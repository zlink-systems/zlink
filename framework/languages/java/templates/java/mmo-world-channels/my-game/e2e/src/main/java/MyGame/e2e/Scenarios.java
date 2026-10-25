package MyGame.e2e;

import MyGame.*;

import java.net.URI;
import java.net.http.*;
import java.util.*;

public final class Scenarios {
    private Scenarios() {}

    private static final long EARNED_GOLD = 10;
    private static final int CHANNEL_TWO = 2;
    private static final String CHANNEL_A1 = "channelA1",
            CHANNEL_A2 = "channelA2",
            CHANNEL_B = "channelB";

    static void run(Scenario scenario, Nodes nodes) throws Exception {
        switch (scenario) {
            case WORLDS -> worlds(nodes);
            case CHANNEL -> channel(nodes);
            case RECONNECT -> reconnect(nodes);
            case UPDATE -> update(nodes);
        }
    }

    private static String player() {
        return "player-" + UUID.randomUUID();
    }

    private static void view(PlayerView value, String world, long channel, String node, long gold) {
        Structure.require(
                value.equals(new PlayerView(world, channel, node, gold)),
                "Unexpected PlayerView: " + value);
    }

    private static void view(Client client, String world, long channel, String node, long gold)
            throws Exception {
        var expected = new PlayerView(world, channel, node, gold);
        view(client.next(PlayerView.class, expected::equals), world, channel, node, gold);
    }

    private static Client current(Client client) {
        client.send(new ViewRequest());
        return client;
    }

    private static void connected(Client client) {
        Structure.require(client.disconnects.get() == 0, "Connection was interrupted");
    }

    private static void sameActor(SessionInfo a, SessionInfo b) {
        Structure.require(
                a.actorId().equals(b.actorId()) && a.objectGeneration() == b.objectGeneration(),
                "Actor identity changed");
    }

    private static void worlds(Nodes n) throws Exception {
        try (var http = HttpClient.newHttpClient()) {
            var reply =
                    http.send(
                            HttpRequest.newBuilder(
                                            URI.create(n.accountsEndpoint + Names.WORLD_LIST_PATH))
                                    .GET()
                                    .build(),
                            HttpResponse.BodyHandlers.ofString());
            Structure.require(reply.statusCode() == 200, "World list HTTP status");
            var list = Json.MAPPER.readValue(reply.body(), WorldInfo[].class);
            Structure.require(list.length == 2, "Expected two worlds");
            for (var w : list)
                Structure.require(
                        w.sessionEndpoint().equals(n.endpoints.get(w.worldId()))
                                && w.name().equals("World " + w.worldId()),
                        "Invalid world entry");
            Structure.require(
                    Arrays.stream(list)
                            .map(WorldInfo::worldId)
                            .collect(java.util.stream.Collectors.toSet())
                            .equals(Set.of(Names.WORLD_A, Names.WORLD_B)),
                    "World identities differ");
        }
        String id = player();
        try (var a = new Client(n.endpoints.get(Names.WORLD_A), id);
                var b = new Client(n.endpoints.get(Names.WORLD_B), id)) {
            view(a.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            a.send(new EarnRequest());
            a.send(new EarnRequest());
            view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            var rejected = a.rejection();
            Structure.require(
                    Rejection.EARN_TOO_SOON.equals(rejected.reason()) && rejected.retryAfterMs() > 0,
                    "Early earn was not rejected with a retry delay");
            view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            a.waitRetryAfter(rejected.retryAfterMs());
            a.send(new EarnRequest());
            view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 2 * EARNED_GOLD);
            view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0);
            view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 2 * EARNED_GOLD);
            connected(a);
            connected(b);
        }
    }

    private static void channel(Nodes n) throws Exception {
        String id = player();
        try (var c = new Client(n.endpoints.get(Names.WORLD_A), id);
                var same = new Client(n.endpoints.get(Names.WORLD_A), id + "-same");
                var moving = new Client(n.endpoints.get(Names.WORLD_A), id + "-moving")) {
            int initialMark = c.markChannelViews();
            view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            c.channelViewAfter(
                    initialMark,
                    value ->
                            value.channelId() == Names.FIRST_CHANNEL
                                    && value.playerIds().equals(List.of(c.playerId)));
            c.send(new EarnRequest());
            view(c, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            int sameAtCMark = c.markChannelViews();
            int sameAtSelfMark = same.markChannelViews();
            view(same.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            var expectedPair = new TreeSet<>(List.of(c.playerId, same.playerId));
            var sameAtC =
                    c.channelViewAfter(
                            sameAtCMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && new TreeSet<>(value.playerIds()).equals(expectedPair));
            var sameAtSelf =
                    same.channelViewAfter(
                            sameAtSelfMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && new TreeSet<>(value.playerIds()).equals(expectedPair));
            Structure.require(
                    new TreeSet<>(sameAtC.playerIds()).equals(expectedPair)
                            && new TreeSet<>(sameAtSelf.playerIds()).equals(expectedPair),
                    "Players in one Zone did not see the same player list");

            int allAtCMark = c.markChannelViews();
            int allAtSameMark = same.markChannelViews();
            int allAtMovingMark = moving.markChannelViews();
            view(moving.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            var expectedAll = new TreeSet<>(List.of(c.playerId, same.playerId, moving.playerId));
            var allAtC =
                    c.channelViewAfter(
                            allAtCMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && new TreeSet<>(value.playerIds()).equals(expectedAll));
            var allAtSame =
                    same.channelViewAfter(
                            allAtSameMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && new TreeSet<>(value.playerIds()).equals(expectedAll));
            moving.channelViewAfter(
                    allAtMovingMark,
                    value ->
                            value.channelId() == Names.FIRST_CHANNEL
                                    && new TreeSet<>(value.playerIds()).equals(expectedAll));
            Structure.require(
                    new TreeSet<>(allAtC.playerIds()).equals(expectedAll)
                            && new TreeSet<>(allAtSame.playerIds()).equals(expectedAll),
                    "Players in one Zone did not see each other");

            c.send(new ChangeChannelRequest(GameRules.MAX_INTEGER + 1));
            var invalidChannel = c.rejection();
            Structure.require(
                    Rejection.INTEGER_OUT_OF_RANGE.equals(invalidChannel.reason())
                            && invalidChannel.retryAfterMs() == 0,
                    "Oversized channel request was not rejected");
            var afterOversizedChannel = c.channelView();
            Structure.require(
                    new TreeSet<>(afterOversizedChannel.playerIds()).equals(expectedAll),
                    "Oversized channel request changed membership");
            c.send(new PlayerView("", 0, "", 0));
            var internal = c.rejection();
            Structure.require(
                    Rejection.PACKET_NOT_ALLOWED.equals(internal.reason())
                            && internal.retryAfterMs() == 0,
                    "Session forwarded an internal packet");

            var identity = c.sessionInfo();
            int oldAtCMark = c.markChannelViews();
            int oldAtSameMark = same.markChannelViews();
            int destinationMark = moving.markChannelViews();
            moving.send(new ChangeChannelRequest(CHANNEL_TWO));
            view(moving, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, 0);
            var oldAtC =
                    c.channelViewAfter(
                            oldAtCMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && new TreeSet<>(value.playerIds()).equals(expectedPair));
            var oldAtSame =
                    same.channelViewAfter(
                            oldAtSameMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && new TreeSet<>(value.playerIds()).equals(expectedPair));
            var movedView =
                    moving.channelViewAfter(
                            destinationMark,
                            value ->
                                    value.channelId() == CHANNEL_TWO
                                            && value.playerIds().equals(List.of(moving.playerId)));
            Structure.require(
                    new TreeSet<>(oldAtC.playerIds()).equals(expectedPair)
                            && new TreeSet<>(oldAtSame.playerIds()).equals(expectedPair)
                            && movedView.playerIds().equals(List.of(moving.playerId)),
                    "Channel move left a player in the old list or exposed another channel");

            int disconnectedMark = c.markChannelViews();
            same.close();
            var afterDisconnect =
                    c.channelViewAfter(
                            disconnectedMark,
                            value ->
                                    value.channelId() == Names.FIRST_CHANNEL
                                            && value.playerIds().equals(List.of(c.playerId)));
            Structure.require(
                    afterDisconnect.playerIds().equals(List.of(c.playerId)),
                    "Disconnected player remained in the channel list");

            var expectedDestination = new TreeSet<>(List.of(c.playerId, moving.playerId));
            int movedMark = c.markChannelViews();
            c.send(new ChangeChannelRequest(CHANNEL_TWO));
            view(c, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD);
            var movedViewForPrimary =
                    c.channelViewAfter(
                            movedMark,
                            value ->
                                    value.channelId() == CHANNEL_TWO
                                            && new TreeSet<>(value.playerIds()).equals(expectedDestination));
            Structure.require(
                    new TreeSet<>(movedViewForPrimary.playerIds()).equals(expectedDestination),
                    "Channel move did not update the destination list");
            Structure.require(identity.equals(c.sessionInfo()), "SessionInfo changed during channel transfer");
            connected(c);
            connected(moving);
        }
    }

    private static void reconnect(Nodes n) throws Exception {
        String id = player();
        SessionInfo identity;
        try (var c = new Client(n.endpoints.get(Names.WORLD_A), id)) {
            view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            c.send(new EarnRequest());
            view(c, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            c.send(new ChangeChannelRequest(CHANNEL_TWO));
            view(c, Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD);
            identity = c.sessionInfo();
        }
        try (var c = new Client(n.endpoints.get(Names.WORLD_A), id)) {
            view(c.connect(), Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD);
            var reconnected = c.sessionInfo();
            sameActor(identity, reconnected);
            Structure.require(
                    !identity.sessionId().equals(reconnected.sessionId()),
                    "New connection reused Session ID");
            connected(c);
        }
    }

    private static void update(Nodes n) throws Exception {
        try (var a = new Client(n.endpoints.get(Names.WORLD_A), player());
                var b = new Client(n.endpoints.get(Names.WORLD_B), player())) {
            int initialMark = a.markChannelViews();
            view(a.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            a.channelViewAfter(
                    initialMark,
                    value ->
                            value.channelId() == Names.FIRST_CHANNEL
                                    && value.playerIds().equals(List.of(a.playerId)));
            view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0);
            a.send(new EarnRequest());
            view(a, Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            var ai = a.sessionInfo();
            var bi = b.sessionInfo();
            n.drain(CHANNEL_A1);
            view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, EARNED_GOLD);
            Structure.require(
                    a.channelView().playerIds().equals(List.of(a.playerId)),
                    "Zone relocation lost its player list");
            view(current(b), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0);
            Structure.require(
                    ai.equals(a.sessionInfo()) && bi.equals(b.sessionInfo()),
                    "Drain changed SessionInfo");
            connected(a);
            connected(b);
            n.replace(CHANNEL_A1);
            view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, EARNED_GOLD);
            view(current(b), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0);
            Structure.require(
                    ai.equals(a.sessionInfo()) && bi.equals(b.sessionInfo()),
                    "Replacement changed SessionInfo");
            n.createUpdateZone(CHANNEL_A1);
            try (var fresh = new Client(n.endpoints.get(Names.WORLD_A), player())) {
                view(fresh.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, 0);
                fresh.send(new ChangeChannelRequest(Names.UPDATE_CHANNEL));
                view(fresh, Names.WORLD_A, Names.UPDATE_CHANNEL, CHANNEL_A1, 0);
                Structure.require(
                        fresh.channelView().playerIds().equals(List.of(fresh.playerId)),
                        "Replacement Zone did not include its joined player");
                connected(fresh);
                fresh.send(new EarnRequest());
                view(fresh, Names.WORLD_A, Names.UPDATE_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            }
            connected(a);
            connected(b);
        }
    }
}
