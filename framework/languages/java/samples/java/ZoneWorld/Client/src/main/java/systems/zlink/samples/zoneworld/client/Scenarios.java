package systems.zlink.samples.zoneworld.client;

import static systems.zlink.samples.zoneworld.client.ScenarioSupport.*;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
import systems.zlink.stream.connector.ZLinkStreamMessage;

final class Scenarios {
    @FunctionalInterface interface Scenario { void run(ClientOptions options) throws Exception; }

    private Scenarios() {}

    static Map<String, Scenario> clientDriven() {
        Map<String, Scenario> all = new LinkedHashMap<>();
        all.put("ZW-A1", Scenarios::a1); all.put("ZW-A2", Scenarios::a2);
        all.put("ZW-A3", Scenarios::a3); all.put("ZW-A4", Scenarios::a4);
        all.put("ZW-A5", Scenarios::a5); all.put("ZW-B1", Scenarios::b1);
        all.put("ZW-B2", Scenarios::b2); all.put("ZW-B3", Scenarios::b3);
        all.put("ZW-B5", Scenarios::b5); all.put("ZW-B6", Scenarios::b6);
        all.put("ZW-B7", Scenarios::b7); all.put("ZW-C1", Scenarios::c1);
        all.put("ZW-C4", Scenarios::c4); all.put("ZW-D1", Scenarios::d1);
        all.put("ZW-E1", Scenarios::e1); all.put("ZW-E2", Scenarios::e2);
        all.put("ZW-E3", Scenarios::e3); all.put("ZW-E4", Scenarios::e4);
        all.put("ZW-E6", Scenarios::e6); all.put("ZW-F1", Scenarios::f1);
        all.put("ZW-F3", Scenarios::f3); all.put("ZW-F4", Scenarios::f4);
        return all;
    }

    static Map<String, Scenario> runnerDriven() {
        Map<String, Scenario> all = new LinkedHashMap<>();
        all.put("ZW-B4", Scenarios::b4); all.put("ZW-B8", Scenarios::b8);
        all.put("ZW-C2", Scenarios::c2); all.put("ZW-C3", Scenarios::c3);
        all.put("ZW-E5-arm", Scenarios::e5Arm); all.put("ZW-E5", Scenarios::e5);
        all.put("ZW-G2", Scenarios::g2); all.put("ZW-G4", Scenarios::g4);
        all.put("ZW-G4-fresh", Scenarios::g4Fresh);
        return all;
    }

    private static void a1(ClientOptions options) {
        try (Game player = new Game(options, unique("a1"))) {
            Messages.JoinWorldRes join = player.join();
            ensure(join.error() == null, "target admission completes before JoinWorldRes");
            ensure("zone-nw".equals(join.zoneId()) && join.x() == 25 && join.y() == 25,
                "canonical spawn is zone-nw (25,25)");
        }
    }

    private static void a2(ClientOptions options) {
        try (Game player = new Game(options, unique("a2"))) {
            player.join();
            player.moveTo(28, 27);
        }
    }

    private static void a3(ClientOptions options) {
        try (Game player = new Game(options, unique("a3"));
             Probes probes = new Probes(options); Ops ops = new Ops(options)) {
            player.join();
            reject(player, -1, 25, "OutOfRange");
            reject(player, 31, 25, "TooFar");
            player.moveTo(49, 49);
            reject(player, 50, 50, "DiagonalCrossing");
            resetMaintenance(ops);
            Messages.RelocationPairRes pair = probes.pair();
            ensure(pair.error() == null, "cross-owner pair exists");
            Edge edge = edge(pair.sourceZoneId(), pair.targetZoneId());
            player.moveTo(edge.source().x(), edge.source().y());
            String node = nodeOwning(ops.watch(), pair.targetZoneId());
            ops.maintenance(node, true);
            try { reject(player, edge.target().x(), edge.target().y(), "ZoneMaintenance"); }
            finally { ops.maintenance(node, false); }
        }
    }

    private static void a4(ClientOptions options) {
        try (Game first = new Game(options, unique("a4-b"));
             Game second = new Game(options, unique("a4-a"))) {
            first.join(); second.join();
            for (Game client : List.of(first, second)) {
                Messages.ZoneStateNotify state = waitFor(client.connector,
                    Messages.ZoneStateNotify.class, value -> has(value, first.playerId)
                        && has(value, second.playerId), Duration.ofSeconds(20))
                    .toCompletableFuture().join().payload();
                ensure(has(state, first.playerId) && has(state, second.playerId),
                    "both clients observe both same-zone players");
            }
        }
    }

    private static void a5(ClientOptions options) {
        String firstId = unique("a5-\uE000");
        String secondId = unique("a5-\uD800\uDC00");
        try (Game first = new Game(options, firstId); Game second = new Game(options, secondId)) {
            first.join(); second.join();
            Messages.ZoneStateNotify state = waitFor(first.connector, Messages.ZoneStateNotify.class,
                value -> has(value, firstId) && has(value, secondId), Duration.ofSeconds(20))
                .toCompletableFuture().join().payload();
            List<String> ids = state.players().stream().map(Messages.PlayerView::playerId).toList();
            ensure(ids.equals(ids.stream().sorted(ZoneWorldSpec.UTF8_ORDER).toList()),
                "Players are UTF-8 byte ordered");
            ensure(state.players().stream().filter(value -> firstId.equals(value.playerId())).count() == 1
                && state.zoneId().equals(state.players().stream()
                    .filter(value -> firstId.equals(value.playerId())).findFirst().orElseThrow().zoneId()),
                "resident value wins over border copy");
        }
    }

    private static void b1(ClientOptions options) {
        try (Game west = new Game(options, unique("b1-w"));
             Game east = new Game(options, unique("b1-e"))) {
            west.join(); east.join();
            east.moveTo(55, 25);
            CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> visible = waitFor(
                east.connector, Messages.ZoneStateNotify.class,
                value -> "zone-ne".equals(value.zoneId()) && has(value, west.playerId),
                Duration.ofSeconds(30));
            west.moveTo(45, 45);
            Messages.ZoneStateNotify neighbor = visible.toCompletableFuture().join().payload();
            Messages.PlayerView view = neighbor.players().stream()
                .filter(value -> west.playerId.equals(value.playerId())).findFirst().orElseThrow();
            ensure("zone-nw".equals(view.zoneId()) && view.x() >= 40,
                "adjacent zone sees the border-band player");
            east.moveTo(55, 55);
            for (int i = 0; i < ZoneWorldSpec.BORDER_EXPIRY_TICKS * 2; i++) {
                Messages.ZoneStateNotify state = waitFor(east.connector,
                    Messages.ZoneStateNotify.class, value -> "zone-se".equals(value.zoneId()),
                    Duration.ofSeconds(30)).toCompletableFuture().join().payload();
                ensure(!has(state, west.playerId), "diagonal zone never sees border snapshot");
            }
        }
    }

    private static void b2(ClientOptions options) {
        String id = unique("b2");
        try (Probes probes = new Probes(options); Game player = new Game(options, id)) {
            Messages.RelocationPairRes pair = requiredPair(probes);
            player.join(); player.moveTo(center(pair.sourceZoneId()).x(), center(pair.sourceZoneId()).y());
            Messages.ActorLocationProbeRes before = probes.actor(id);
            player.moveTo(center(pair.targetZoneId()).x(), center(pair.targetZoneId()).y());
            Messages.ActorLocationProbeRes after = probes.actor(id);
            ensure(before.ownerNodeRid().equals(pair.sourceOwnerNodeRid())
                && after.ownerNodeRid().equals(pair.targetOwnerNodeRid())
                && !before.ownerNodeRid().equals(after.ownerNodeRid()), "actor crosses owners");
            Point target = center(pair.targetZoneId());
            player.moveTo(target.x() + (target.x() < 50 ? 1 : -1), target.y());
        }
        try (Game resumed = new Game(options, id)) {
            ensure(resumed.join().error() == null, "relocated identity rebinds");
        }
    }

    private static void b3(ClientOptions options) {
        String id = unique("b3");
        try (Probes probes = new Probes(options); Game player = new Game(options, id)) {
            Messages.RelocationPairRes pair = requiredPair(probes); player.join();
            Point source = center(pair.sourceZoneId()), target = center(pair.targetZoneId());
            player.moveTo(source.x(), source.y()); Messages.ActorLocationProbeRes before = probes.actor(id);
            player.moveTo(target.x(), target.y()); Messages.ActorLocationProbeRes after = probes.actor(id);
            ensure(before.actorId().equals(after.actorId())
                && before.objectGeneration() == after.objectGeneration()
                && !before.ownerNodeRid().equals(after.ownerNodeRid()),
                "relocation preserves actor identity and advances owner");
        }
    }

    private static Follow preparedFollow(String prefix, ClientOptions options) {
        Probes probes = new Probes(options); Game player = new Game(options, unique(prefix));
        Messages.RelocationPairRes pair = requiredPair(probes); player.join();
        Point source = center(pair.sourceZoneId()), target = center(pair.targetZoneId());
        player.moveTo(source.x(), source.y()); Messages.ActorLocationProbeRes before = probes.actor(player.playerId);
        byte[] prime = "route-prime".getBytes(StandardCharsets.UTF_8);
        Messages.MessageFollowProbeRes primed = probes.probe(player.playerId, "prime-" + player.playerId, prime);
        ensure(Arrays.equals(prime, primed.payload()), "old owner route is primed");
        player.moveTo(target.x(), target.y()); Messages.ActorLocationProbeRes after = probes.actor(player.playerId);
        ensure(after.objectGeneration() == before.objectGeneration()
            && !after.ownerNodeRid().equals(before.ownerNodeRid()), "probe actor relocated");
        return new Follow(probes, player, after.objectGeneration());
    }

    private static void b5(ClientOptions options) {
        Follow value = preparedFollow("b5", options);
        Probes probes = value.probes(); Game player = value.player();
        try (probes; player) {
            String id = "one-way-" + unique("b5");
            value.probes().sendProbe(value.player().playerId, id,
                "one-way-payload".getBytes(StandardCharsets.UTF_8));
            System.out.println("message-follow-one-way completed actor=" + value.player().playerId
                + " probe=" + id + " generation=" + value.generation());
        }
    }

    private static void b6(ClientOptions options) {
        Follow value = preparedFollow("b6", options);
        Probes probes = value.probes(); Game player = value.player();
        try (probes; player) {
            String id = "request-" + unique("b6"); byte[] payload = "request-payload".getBytes(StandardCharsets.UTF_8);
            Messages.MessageFollowProbeRes reply = value.probes().probe(value.player().playerId, id, payload);
            ensure(id.equals(reply.probeId()) && Arrays.equals(payload, reply.payload()),
                "followed request preserves payload and reply route");
            System.out.println("message-follow-request completed actor=" + value.player().playerId
                + " request=" + id + " generation=" + value.generation());
        }
    }

    private static void b7(ClientOptions options) {
        String id = unique("b7");
        try (Probes probes = new Probes(options); Game player = new Game(options, id)) {
            Messages.RelocationPairRes pair = requiredPair(probes); player.join();
            Point source = center(pair.sourceZoneId()), target = center(pair.targetZoneId());
            player.moveTo(source.x(), source.y()); Messages.ActorLocationProbeRes home = probes.actor(id);
            player.moveTo(target.x(), target.y()); Messages.ActorLocationProbeRes away = probes.actor(id);
            Messages.ZoneChangedNotify returned = player.moveTo(source.x(), source.y());
            player.moveTo(source.x(), source.y() + 3); Messages.ActorLocationProbeRes back = probes.actor(id);
            ensure(returned != null && id.equals(returned.playerId())
                && home.objectGeneration() == away.objectGeneration()
                && home.objectGeneration() == back.objectGeneration()
                && home.ownerNodeRid().equals(back.ownerNodeRid()), "A-B-A identity and binding survive");
        }
    }

    private static void c1(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            Messages.WatchNodesRes nodes = ops.watch();
            ensure(nodes.nodes().stream().filter(value -> value.registered() && value.connected()).count() >= 2,
                "two ZoneNodes are independently registered and connected");
        }
    }

    private static void c4(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            CompletionStage<ZLinkStreamMessage<Messages.NodeAlertNotify>> alert = waitFor(
                ops.connector, Messages.NodeAlertNotify.class,
                value -> "TimerHandlerFailed".equals(value.kind()), Duration.ofSeconds(40));
            ops.watch();
            Messages.NodeAlertNotify observed = alert.toCompletableFuture().join().payload();
            ensure(ops.watch().nodes().stream().anyMatch(value -> value.nodeId().equals(observed.nodeId())),
                "timer failure alert names a current node");
        }
    }

    private static void d1(ClientOptions options) {
        List<Game> players = new ArrayList<>();
        try {
            for (String zone : ZoneWorldSpec.zones()) {
                Game player = new Game(options, unique("d1-" + zone)); players.add(player);
                player.join(); Point point = center(zone); player.moveTo(point.x(), point.y());
            }
            List<CompletionStage<ZLinkStreamMessage<Messages.WorldAnnounceNotify>>> waits = players.stream()
                .map(player -> waitFor(player.connector, Messages.WorldAnnounceNotify.class,
                    value -> true, Duration.ofSeconds(15))).toList();
            try (Ops ops = new Ops(options)) {
                Messages.AnnounceWorldRes published = request(ops.connector,
                    new Messages.AnnounceWorldReq("maintenance in 10 minutes"),
                    Messages.AnnounceWorldRes.class);
                for (CompletionStage<ZLinkStreamMessage<Messages.WorldAnnounceNotify>> wait : waits)
                    ensure(published.announcementId().equals(wait.toCompletableFuture().join().payload().announcementId()),
                        "every zone client receives the same announcement");
                for (Game player : players) player.connector.expectNone(Messages.WorldAnnounceNotify.class)
                    .within(Duration.ofSeconds(3)).submit().toCompletableFuture().join();
            }
        } finally { players.forEach(Game::close); }
    }

    private static void e1(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            resetMaintenance(ops); Messages.WatchNodesRes before = ops.watch();
            Messages.NodeView target = before.nodes().stream().filter(Messages.NodeView::registered)
                .max(Comparator.comparing(Messages.NodeView::nodeId)).orElseThrow();
            Map<String, Boolean> others = before.nodes().stream().filter(value -> !value.nodeId().equals(target.nodeId()))
                .collect(java.util.stream.Collectors.toMap(Messages.NodeView::nodeId, Messages.NodeView::maintenance));
            Messages.SetMaintenanceRes set = ops.maintenance(target.nodeId(), true);
            try {
                ensure(set.error() == null && set.enabled(), "target desired state is stored");
                Messages.WatchNodesRes after = ops.watch();
                ensure(after.nodes().stream().filter(value -> value.nodeId().equals(target.nodeId()))
                    .findFirst().orElseThrow().maintenance(), "target alone enters maintenance");
                for (var entry : others.entrySet()) ensure(after.nodes().stream()
                    .filter(value -> value.nodeId().equals(entry.getKey())).findFirst().orElseThrow().maintenance()
                    == entry.getValue(), "non-target maintenance is unchanged");
            } finally { ops.maintenance(target.nodeId(), false); }
        }
    }

    private static void e2(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            resetMaintenance(ops); String node = nodeOwning(ops.watch(), "zone-nw");
            ops.maintenance(node, true);
            try (Game player = new Game(options, unique("e2"))) {
                ensure("ZoneMaintenance".equals(player.join().error()),
                    "target OnActorJoin rejects a new entry");
            } finally { ops.maintenance(node, false); }
        }
    }

    private static void e3(ClientOptions options) {
        try (Ops ops = new Ops(options); Game player = new Game(options, unique("e3"))) {
            resetMaintenance(ops); player.join(); String node = nodeOwning(ops.watch(), "zone-nw");
            ops.maintenance(node, true);
            try { player.moveTo(30, 30); }
            finally { ops.maintenance(node, false); }
        }
    }

    private static void e4(ClientOptions options) {
        try (Ops ops = new Ops(options); Game player = new Game(options, unique("e4"))) {
            resetMaintenance(ops); Messages.WatchNodesRes nodes = ops.watch();
            List<List<String>> pairs = List.of(List.of("zone-nw", "zone-ne"), List.of("zone-nw", "zone-sw"),
                List.of("zone-ne", "zone-se"), List.of("zone-sw", "zone-se"));
            List<String> selected = pairs.stream().filter(pair -> nodes.nodes().stream().anyMatch(node ->
                node.registered() && node.zones().containsAll(pair))).findFirst().orElseThrow();
            String node = nodes.nodes().stream().filter(value -> value.zones().containsAll(selected))
                .findFirst().orElseThrow().nodeId(); Edge edge = edge(selected.get(0), selected.get(1));
            player.join(); player.moveTo(edge.source().x(), edge.source().y()); ops.maintenance(node, true);
            try { reject(player, edge.target().x(), edge.target().y(), "ZoneMaintenance"); }
            finally { ops.maintenance(node, false); }
        }
    }

    private static void e6(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            Messages.NodeView target = ops.watch().nodes().stream().filter(Messages.NodeView::registered)
                .findFirst().orElseThrow();
            Messages.NodeDiagnosticsRes result = request(ops.connector,
                new Messages.NodeDiagnosticsReq(target.nodeId()), Messages.NodeDiagnosticsRes.class);
            ensure(result.error() == null && result.nodeId().equals(target.nodeId())
                && !result.zones().isEmpty() && result.playerCount() >= 0,
                "diagnostics returns current zones, count and maintenance");
        }
    }

    private static void f1(ClientOptions options) {
        try (Game player = new Game(options, unique("f1"))) {
            CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> first = waitFor(
                player.connector, Messages.ZoneStateNotify.class,
                value -> value.players().stream().anyMatch(Messages.PlayerView::isBot), Duration.ofSeconds(30));
            player.join(); Messages.ZoneStateNotify state = first.toCompletableFuture().join().payload();
            Map<String, Point> bots = new java.util.HashMap<>();
            state.players().stream().filter(Messages.PlayerView::isBot).forEach(value ->
                bots.put(value.playerId(), new Point(value.x(), value.y())));
            waitFor(player.connector, Messages.ZoneStateNotify.class, value -> value.players().stream()
                .anyMatch(bot -> bot.isBot() && bots.containsKey(bot.playerId())
                    && (bots.get(bot.playerId()).x() != bot.x() || bots.get(bot.playerId()).y() != bot.y())),
                Duration.ofSeconds(30)).toCompletableFuture().join();
        }
    }

    private static void f3(ClientOptions options) {
        try (Ops ops = new Ops(options); Game player = new Game(options, unique("f3"))) {
            resetMaintenance(ops); player.join();
            Messages.ZoneStateNotify boundary = waitFor(player.connector, Messages.ZoneStateNotify.class,
                value -> aboutToCross(value) != null, Duration.ofSeconds(45)).toCompletableFuture().join().payload();
            Messages.PlayerView bot = aboutToCross(boundary); String targetZone = bot.zoneId().equals("zone-nw")
                ? "zone-ne" : "zone-nw"; String node = nodeOwning(ops.watch(), targetZone);
            ops.maintenance(node, true);
            try {
                int initial = bot.x(); boolean east = bot.zoneId().equals("zone-nw");
                Messages.ZoneStateNotify reversed = waitFor(player.connector, Messages.ZoneStateNotify.class,
                    value -> value.players().stream().anyMatch(candidate -> candidate.playerId().equals(bot.playerId())
                        && (east ? candidate.x() < initial : candidate.x() > initial)), Duration.ofSeconds(45))
                    .toCompletableFuture().join().payload();
                ensure(reversed.players().stream().anyMatch(value -> value.playerId().equals(bot.playerId())),
                    "rejected bot reverses direction");
            } finally { ops.maintenance(node, false); }
        }
    }

    private static void f4(ClientOptions options) {
        try (Game player = new Game(options, unique("f4")); Ops ops = new Ops(options)) {
            player.join(); request(ops.connector, new Messages.AnnounceWorldReq("bots receive nothing"),
                Messages.AnnounceWorldRes.class); reject(player, -40, player.y, "OutOfRange");
            ensure(waitFor(player.connector, Messages.ZoneStateNotify.class,
                value -> value.players().stream().anyMatch(Messages.PlayerView::isBot), Duration.ofSeconds(20))
                .toCompletableFuture().join().payload().players().stream().anyMatch(Messages.PlayerView::isBot),
                "no-push traffic runs alongside bots");
        }
    }

    private static void b4(ClientOptions options) {
        try (Probes probes = new Probes(options); Ops ops = new Ops(options)) {
            Messages.RelocationPairRes pair = requiredPair(probes); Edge edge = edge(pair.sourceZoneId(), pair.targetZoneId());
            try (Game source = new Game(options, unique("b4-source"));
                 Game target = new Game(options, unique("b4-target"))) {
                source.join(); target.join(); source.moveTo(edge.source().x(), edge.source().y());
                CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> visible = waitFor(source.connector,
                    Messages.ZoneStateNotify.class, value -> value.zoneId().equals(pair.sourceZoneId())
                        && has(value, target.playerId), Duration.ofSeconds(30));
                target.moveTo(edge.target().x(), edge.target().y()); visible.toCompletableFuture().join();
                String node = nodeOwning(ops.watch(), pair.targetZoneId());
                CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> expired = waitFor(source.connector,
                    Messages.ZoneStateNotify.class, value -> value.zoneId().equals(pair.sourceZoneId())
                        && !has(value, target.playerId), Duration.ofSeconds(60));
                System.out.println("scenario ZW-B4 armed node=" + node);
                expired.toCompletableFuture().join();
            }
        }
    }

    private static void c2(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            String node = ops.watch().nodes().stream().filter(Messages.NodeView::connected)
                .max(Comparator.comparing(Messages.NodeView::nodeId)).orElseThrow().nodeId();
            CompletionStage<ZLinkStreamMessage<Messages.NodeStatusNotify>> dropped = waitFor(ops.connector,
                Messages.NodeStatusNotify.class, value -> value.nodeId().equals(node) && !value.connected(),
                Duration.ofSeconds(60));
            System.out.println("scenario ZW-C2 armed node=" + node);
            ensure(!dropped.toCompletableFuture().join().payload().connected(), "runtime event reports disconnect");
        }
    }

    private static void c3(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            String node = ops.watch().nodes().stream().filter(Messages.NodeView::registered)
                .max(Comparator.comparing(Messages.NodeView::nodeId)).orElseThrow().nodeId();
            CompletionStage<ZLinkStreamMessage<Messages.NodeStatusNotify>> expired = waitFor(ops.connector,
                Messages.NodeStatusNotify.class, value -> value.nodeId().equals(node) && !value.registered(),
                Duration.ofSeconds(60));
            System.out.println("scenario ZW-C3 armed node=" + node);
            ensure(!expired.toCompletableFuture().join().payload().registered(), "report expires after TTL");
        }
    }

    private static void e5Arm(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            String node = ops.watch().nodes().stream().filter(Messages.NodeView::registered)
                .max(Comparator.comparing(Messages.NodeView::nodeId)).orElseThrow().nodeId();
            Messages.SetMaintenanceRes result = ops.maintenance(node, true);
            ensure(result.enabled(), "maintenance is stored before restart");
            System.out.println("scenario ZW-E5-arm armed node=" + node);
        }
    }

    private static void e5(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            Messages.NodeView node = ops.watch().nodes().stream().filter(value -> value.registered()
                && value.connected() && value.maintenance()).findFirst().orElseThrow();
            Messages.NodeDiagnosticsRes diagnostics = request(ops.connector,
                new Messages.NodeDiagnosticsReq(node.nodeId()), Messages.NodeDiagnosticsRes.class);
            try { ensure(diagnostics.error() == null && diagnostics.maintenance(),
                "restart restores stored maintenance"); }
            finally { ops.maintenance(node.nodeId(), false); }
        }
    }

    private static void g2(ClientOptions options) {
        try (Ops ops = new Ops(options)) {
            Messages.NodeView node = ops.watch().nodes().stream().filter(value -> value.registered() && value.connected())
                .findFirst().orElseThrow(); resetMaintenance(ops);
            ops.maintenance(node.nodeId(), true);
            try {
                Messages.NodeDiagnosticsRes diagnostics = request(ops.connector,
                    new Messages.NodeDiagnosticsReq(node.nodeId()), Messages.NodeDiagnosticsRes.class);
                ensure(diagnostics.error() == null && diagnostics.nodeId().equals(node.nodeId()),
                    "reverse-started node accepts operations");
            } finally { ops.maintenance(node.nodeId(), false); }
        }
    }

    private static void g4(ClientOptions options) {
        try (Ops ops = new Ops(options); Probes probes = new Probes(options)) {
            Messages.NodeView east = ops.watch().nodes().stream()
                .filter(value -> value.nodeId().equals("zone-node-2")).findFirst().orElseThrow();
            Messages.RelocationPairRes observed = requiredPair(probes);
            Messages.RelocationPairRes pair = east.zones().contains(observed.targetZoneId()) ? observed
                : new Messages.RelocationPairRes(observed.targetZoneId(), observed.sourceZoneId(),
                    observed.targetOwnerNodeRid(), observed.sourceOwnerNodeRid(), null);
            Edge edge = edge(pair.sourceZoneId(), pair.targetZoneId());
            try (Game player = new Game(options, unique("g4-crash"))) {
                player.join(); player.moveTo(edge.source().x(), edge.source().y());
                CompletionStage<ZLinkStreamMessage<Messages.CrashRelocationProbeRes>> failed = waitFor(
                    player.connector, Messages.CrashRelocationProbeRes.class,
                    value -> "Unavailable".equals(value.error()), Duration.ofSeconds(60));
                player.connector.send(new Messages.CrashRelocationProbeMsg(edge.target().x(), edge.target().y()))
                    .submit().toCompletableFuture().join();
                System.out.println("scenario ZW-G4 armed node=zone-node-2");
                ensure("Unavailable".equals(failed.toCompletableFuture().join().payload().error()),
                    "crashed Ready owner terminates in-flight operation Unavailable");
            }
        }
    }

    private static void g4Fresh(ClientOptions options) {
        try (Probes probes = new Probes(options)) {
            for (int i = 0; i < 16; i++) {
                Messages.FreshActorProbeRes created = probes.fresh(unique("g4-fresh"));
                ensure(created.error() == null && created.objectGeneration() > 0,
                    "replacement accepts a fresh actor");
                System.out.println("scenario ZW-G4-fresh owner=" + created.ownerNodeRid()
                    + " actor=" + created.actorId());
            }
        }
    }

    private static void b8(ClientOptions options) throws Exception {
        try (Probes probes = new Probes(options)) {
            Messages.RelocationPairRes pair = requiredPair(probes); Edge edge = edge(pair.sourceZoneId(), pair.targetZoneId());
            String id = unique("b8-seal");
            try (Game player = new Game(options, id)) {
                player.join(); player.moveTo(edge.source().x(), edge.source().y());
                CompletableFuture<String> disconnected = new CompletableFuture<>();
                player.connector.onDisconnected(event -> {
                    disconnected.complete(event.closeReason().toString());
                    return CompletableFuture.completedFuture(null);
                });
                System.out.println("scenario ZW-B8 armed");
                Path arm = Path.of(options.faultArmFile());
                for (int i = 0; !Files.exists(arm); i++) {
                    if (i >= 200) throw new IllegalStateException("runner did not arm command 44 block");
                    delay(Duration.ofMillis(50));
                }
                player.move(edge.target().x(), edge.target().y());
                String reason = disconnected.orTimeout(60, java.util.concurrent.TimeUnit.SECONDS).join();
                System.out.println("scenario ZW-B8 disconnected reason=" + reason);
                player.connector.connect().submit().toCompletableFuture().join();
                Messages.JoinWorldRes rebound = player.join();
                ensure(id.equals(rebound.playerId()) && pair.targetZoneId().equals(rebound.zoneId()),
                    "reconnect rebinds the relocated actor");
            }
        }
    }

    private static Messages.RelocationPairRes requiredPair(Probes probes) {
        Messages.RelocationPairRes pair = probes.pair();
        ensure(pair.error() == null, "probe discovers a cross-owner adjacent pair");
        return pair;
    }

    private static String nodeOwning(Messages.WatchNodesRes nodes, String zone) {
        return nodes.nodes().stream().filter(value -> value.zones().contains(zone))
            .findFirst().orElseThrow().nodeId();
    }

    private static void resetMaintenance(Ops ops) {
        for (Messages.NodeView node : ops.watch().nodes()) if (node.registered()) ops.maintenance(node.nodeId(), false);
    }

    private static void reject(Game player, int x, int y, String reason) {
        CompletionStage<ZLinkStreamMessage<Messages.MoveRejectedNotify>> waiting = waitFor(
            player.connector, Messages.MoveRejectedNotify.class, value -> true, Duration.ofSeconds(20));
        int beforeX = player.x, beforeY = player.y; player.move(x, y);
        Messages.MoveRejectedNotify rejected = waiting.toCompletableFuture().join().payload();
        ensure(reason.equals(rejected.reason()) && rejected.x() == beforeX && rejected.y() == beforeY,
            "rejection reason/order and unchanged coordinate: " + reason);
    }

    private static boolean has(Messages.ZoneStateNotify state, String playerId) {
        return state.players().stream().anyMatch(value -> playerId.equals(value.playerId()));
    }

    private static Messages.PlayerView aboutToCross(Messages.ZoneStateNotify state) {
        return state.players().stream().filter(value -> value.isBot() && value.playerId().endsWith("-x")
            && (("zone-nw".equals(value.zoneId()) && value.x() + ZoneWorldSpec.BOT_STEP >= 50)
                || ("zone-ne".equals(value.zoneId()) && value.x() - ZoneWorldSpec.BOT_STEP < 50)))
            .findFirst().orElse(null);
    }

    private record Follow(Probes probes, Game player, long generation) {}
}
