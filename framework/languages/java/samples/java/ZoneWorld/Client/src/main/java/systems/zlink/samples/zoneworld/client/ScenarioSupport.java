package systems.zlink.samples.zoneworld.client;

import java.net.URI;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Predicate;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamMessage;

final class ScenarioSupport {
    static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(15);
    static final Duration CROSS_NODE_TIMEOUT = Duration.ofSeconds(30);

    private ScenarioSupport() {}

    static ZLinkStreamConnector connect(String endpoint) {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create(endpoint), ZLinkStreamDispatchMode.IMMEDIATE,
                REQUEST_TIMEOUT, REQUEST_TIMEOUT, 2, Duration.ofSeconds(5),
                64 * 1024, 64 * 1024, Integer.MAX_VALUE, 1024, 0, true,
                Duration.ofSeconds(1), Duration.ofSeconds(5), true,
                Duration.ofMillis(250), Duration.ofSeconds(5), 2.0, false,
                null, null, null, null));
        connector.connect().submit().toCompletableFuture().join();
        return connector;
    }

    static <T> CompletionStage<ZLinkStreamMessage<T>> waitFor(
        ZLinkStreamConnector connector, Class<T> type, Predicate<T> predicate, Duration timeout) {
        return connector.waitFor(type).where(type, message -> predicate.test(message.payload()))
            .timeout(timeout).submit(type);
    }

    static <T> T request(ZLinkStreamConnector connector, Object request, Class<T> reply) {
        return connector.request(request).timeout(REQUEST_TIMEOUT).submit(reply)
            .toCompletableFuture().join();
    }

    static void delay(Duration duration) {
        CompletableFuture.runAsync(() -> {},
            CompletableFuture.delayedExecutor(duration.toMillis(), TimeUnit.MILLISECONDS)).join();
    }

    static void close(ZLinkStreamConnector connector) {
        if (connector != null) connector.close().submit().toCompletableFuture().join();
    }

    static void ensure(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }

    static String unique(String prefix) {
        return prefix + "-" + java.util.UUID.randomUUID().toString().replace("-", "").substring(0, 6);
    }

    static Point center(String zone) {
        return switch (zone) {
            case "zone-nw" -> new Point(25, 25);
            case "zone-ne" -> new Point(75, 25);
            case "zone-sw" -> new Point(25, 75);
            case "zone-se" -> new Point(75, 75);
            default -> throw new IllegalArgumentException("unknown zone " + zone);
        };
    }

    static Edge edge(String source, String target) {
        return switch (source + ":" + target) {
            case "zone-nw:zone-ne" -> new Edge(new Point(48, 25), new Point(52, 25));
            case "zone-nw:zone-sw" -> new Edge(new Point(25, 48), new Point(25, 52));
            case "zone-ne:zone-se" -> new Edge(new Point(75, 48), new Point(75, 52));
            case "zone-sw:zone-se" -> new Edge(new Point(48, 75), new Point(52, 75));
            case "zone-ne:zone-nw" -> new Edge(new Point(52, 25), new Point(48, 25));
            case "zone-sw:zone-nw" -> new Edge(new Point(25, 52), new Point(25, 48));
            case "zone-se:zone-ne" -> new Edge(new Point(75, 52), new Point(75, 48));
            case "zone-se:zone-sw" -> new Edge(new Point(52, 75), new Point(48, 75));
            default -> throw new IllegalArgumentException("zones are not adjacent");
        };
    }

    record Point(int x, int y) {}
    record Edge(Point source, Point target) {}

    static final class Game implements AutoCloseable {
        final ZLinkStreamConnector connector;
        final String playerId;
        int x;
        int y;

        Game(ClientOptions options, String playerId) {
            this.connector = connect(options.gatewayEndpoint());
            this.playerId = playerId;
        }

        Messages.JoinWorldNotify join() {
            CompletionStage<ZLinkStreamMessage<Messages.JoinWorldNotify>> response = waitFor(
                connector, Messages.JoinWorldNotify.class, value -> playerId.equals(value.playerId()),
                Duration.ofSeconds(20));
            connector.send(new Messages.JoinWorldMsg(playerId)).submit().toCompletableFuture().join();
            Messages.JoinWorldNotify value = response.toCompletableFuture().join().payload();
            x = value.x(); y = value.y();
            return value;
        }

        void move(int targetX, int targetY) {
            connector.send(new Messages.MoveMsg(targetX, targetY)).submit().toCompletableFuture().join();
        }

        Messages.ZoneChangedNotify moveTo(int targetX, int targetY) {
            Messages.ZoneChangedNotify last = null;
            while (x != targetX || y != targetY) {
                int nextX = x != targetX ? x + Math.clamp(targetX - x,
                    -ZoneWorldSpec.MAX_STEP_PER_AXIS, ZoneWorldSpec.MAX_STEP_PER_AXIS) : x;
                int nextY = x == targetX ? y + Math.clamp(targetY - y,
                    -ZoneWorldSpec.MAX_STEP_PER_AXIS, ZoneWorldSpec.MAX_STEP_PER_AXIS) : y;
                String oldZone = ZoneWorldSpec.zoneOf(x, y);
                String newZone = ZoneWorldSpec.zoneOf(nextX, nextY);
                if (!oldZone.equals(newZone)) {
                    CompletionStage<ZLinkStreamMessage<Messages.ZoneChangedNotify>> changed =
                        waitFor(connector, Messages.ZoneChangedNotify.class,
                            value -> playerId.equals(value.playerId()) && newZone.equals(value.zoneId()),
                            CROSS_NODE_TIMEOUT);
                    move(nextX, nextY);
                    last = changed.toCompletableFuture().join().payload();
                } else {
                    CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> arrived =
                        waitFor(connector, Messages.ZoneStateNotify.class,
                            state -> state.players().stream().anyMatch(player ->
                                playerId.equals(player.playerId())
                                    && player.x() == nextX && player.y() == nextY),
                            CROSS_NODE_TIMEOUT);
                    move(nextX, nextY);
                    arrived.toCompletableFuture().join();
                }
                x = nextX; y = nextY;
            }
            return last;
        }

        @Override public void close() { ScenarioSupport.close(connector); }
    }

    static final class Ops implements AutoCloseable {
        final ZLinkStreamConnector connector;
        Ops(ClientOptions options) { connector = connect(options.opsEndpoint()); }
        Messages.WatchNodesRes watch() { return request(connector,
            new Messages.WatchNodesReq(), Messages.WatchNodesRes.class); }
        Messages.SetMaintenanceRes maintenance(String nodeId, boolean enabled) {
            CompletionStage<ZLinkStreamMessage<Messages.NodeStatusNotify>> observed = waitFor(
                connector, Messages.NodeStatusNotify.class,
                value -> nodeId.equals(value.nodeId()) && value.maintenance() == enabled,
                Duration.ofSeconds(20));
            Messages.SetMaintenanceRes result = request(connector,
                new Messages.SetMaintenanceReq(nodeId, enabled), Messages.SetMaintenanceRes.class);
            if (result.error() == null) observed.toCompletableFuture().join();
            return result;
        }
        @Override public void close() { ScenarioSupport.close(connector); }
    }

    static final class Probes implements AutoCloseable {
        final ZLinkStreamConnector connector;
        Probes(ClientOptions options) { connector = connect(options.gatewayEndpoint()); }
        Messages.RelocationPairRes pair() { return request(connector,
            new Messages.RelocationPairReq(), Messages.RelocationPairRes.class); }
        Messages.ActorLocationProbeRes actor(String id) { return request(connector,
            new Messages.ActorLocationProbeReq(id), Messages.ActorLocationProbeRes.class); }
        Messages.FreshActorProbeRes fresh(String id) { return request(connector,
            new Messages.FreshActorProbeReq(id), Messages.FreshActorProbeRes.class); }
        Messages.MessageFollowProbeRes probe(String actor, String id, byte[] payload) {
            return request(connector, new Messages.MessageFollowProbeReq(actor, id, payload),
                Messages.MessageFollowProbeRes.class);
        }
        void sendProbe(String actor, String id, byte[] payload) {
            connector.send(new Messages.MessageFollowProbeMsg(actor, id, payload))
                .submit().toCompletableFuture().join();
        }
        @Override public void close() { ScenarioSupport.close(connector); }
    }
}
