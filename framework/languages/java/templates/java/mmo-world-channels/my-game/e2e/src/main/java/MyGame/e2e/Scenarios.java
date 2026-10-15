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

    private static void view(PlayerView value, String world, int channel, String node, long gold) {
        Structure.require(
                value.equals(new PlayerView(world, channel, node, gold)),
                "Unexpected PlayerView: " + value);
    }

    private static PlayerView current(Client client) throws Exception {
        client.send(new ViewRequest());
        return client.next(PlayerView.class);
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
            a.send(new EarnRequest(EARNED_GOLD));
            view(
                    a.next(PlayerView.class),
                    Names.WORLD_A,
                    Names.FIRST_CHANNEL,
                    CHANNEL_A1,
                    EARNED_GOLD);
            view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0);
            view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, EARNED_GOLD);
            connected(a);
            connected(b);
        }
    }

    private static void channel(Nodes n) throws Exception {
        try (var c = new Client(n.endpoints.get(Names.WORLD_A), player())) {
            view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            c.send(new EarnRequest(EARNED_GOLD));
            view(
                    c.next(PlayerView.class),
                    Names.WORLD_A,
                    Names.FIRST_CHANNEL,
                    CHANNEL_A1,
                    EARNED_GOLD);
            var identity = c.sessionInfo();
            c.send(new ChangeChannelRequest(CHANNEL_TWO));
            view(c.next(PlayerView.class), Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD);
            Structure.require(
                    identity.equals(c.sessionInfo()),
                    "SessionInfo changed during channel transfer");
            connected(c);
        }
    }

    private static void reconnect(Nodes n) throws Exception {
        String id = player();
        SessionInfo identity;
        try (var c = new Client(n.endpoints.get(Names.WORLD_A), id)) {
            view(c.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            c.send(new EarnRequest(EARNED_GOLD));
            view(
                    c.next(PlayerView.class),
                    Names.WORLD_A,
                    Names.FIRST_CHANNEL,
                    CHANNEL_A1,
                    EARNED_GOLD);
            c.send(new ChangeChannelRequest(CHANNEL_TWO));
            view(c.next(PlayerView.class), Names.WORLD_A, CHANNEL_TWO, CHANNEL_A2, EARNED_GOLD);
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
            view(a.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A1, 0);
            view(b.connect(), Names.WORLD_B, Names.FIRST_CHANNEL, CHANNEL_B, 0);
            a.send(new EarnRequest(EARNED_GOLD));
            view(
                    a.next(PlayerView.class),
                    Names.WORLD_A,
                    Names.FIRST_CHANNEL,
                    CHANNEL_A1,
                    EARNED_GOLD);
            var ai = a.sessionInfo();
            var bi = b.sessionInfo();
            n.drain(CHANNEL_A1);
            view(current(a), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, EARNED_GOLD);
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
            try (var fresh = new Client(n.endpoints.get(Names.WORLD_A), player())) {
                view(fresh.connect(), Names.WORLD_A, Names.FIRST_CHANNEL, CHANNEL_A2, 0);
                n.accepted(CHANNEL_A1, fresh.playerId);
                connected(fresh);
                fresh.send(new EarnRequest(EARNED_GOLD));
                view(
                        fresh.next(PlayerView.class),
                        Names.WORLD_A,
                        Names.FIRST_CHANNEL,
                        CHANNEL_A2,
                        EARNED_GOLD);
            }
            connected(a);
            connected(b);
        }
    }
}
