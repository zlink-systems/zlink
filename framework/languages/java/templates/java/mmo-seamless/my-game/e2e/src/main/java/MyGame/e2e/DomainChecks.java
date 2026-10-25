package MyGame.e2e;

import MyGame.*;
import MyGame.zone.ZonePositionDomain;

import java.util.concurrent.atomic.AtomicLong;

public final class DomainChecks {
    private int assertions;

    private DomainChecks() {}

    private void require(boolean condition, String message) {
        assertions++;
        if (!condition) throw new IllegalStateException(message);
    }

    private ZonePositionDomain create(AtomicLong now) {
        var domain = new ZonePositionDomain("zone-2", now::get);
        domain.addPlayer("player", new EnterZone(new Position(100), 0, 0));
        return domain;
    }

    private void requireRejected(
            ZonePositionDomain domain, double delta, long sequence, String reason, String playerId) {
        var before = domain.state(playerId);
        var result = domain.move(playerId, new MoveRequest(delta, sequence));
        require(result.rejection() != null, "Expected " + reason + " rejection");
        require(reason.equals(result.rejection().reason()), "Expected " + reason + " rejection");
        require(result.rejection().retryAfterMs() == 0, reason + " must not report a retry delay");
        require(before.equals(domain.state(playerId)), "Rejected input changed domain state");
    }

    private void nonFiniteMovement() {
        var domain = create(new AtomicLong());
        requireRejected(domain, Double.NaN, 1, "non-finite", "player");
        requireRejected(domain, Double.POSITIVE_INFINITY, 1, "non-finite", "player");
        requireRejected(domain, Double.NEGATIVE_INFINITY, 1, "non-finite", "player");
    }

    private void stepLimit() {
        requireRejected(create(new AtomicLong()), Rules.MAX_STEP + 1, 1, "step-too-large", "player");
    }

    private void worldBoundary() {
        var domain = new ZonePositionDomain("zone-6", () -> 0);
        domain.addPlayer("edge", new EnterZone(new Position(Rules.ZONE_WIDTH * (double) Rules.ZONE_COUNT - 1), 0, 0));
        requireRejected(domain, 1, 1, "outside-world", "edge");
    }

    private void movementBudget() {
        var now = new AtomicLong();
        var domain = create(now);
        var first = domain.move("player", new MoveRequest(Rules.MAX_STEP, 1));
        require(first.accepted() && first.position().x() == 110, "First move must have MaxStep budget");
        var before = domain.state("player");
        var rejected = domain.move("player", new MoveRequest(Rules.MAX_STEP, 2));
        require(rejected.rejection() != null, "Expected movement budget rejection");
        require("move-budget".equals(rejected.rejection().reason()), "Expected move-budget rejection");
        require(rejected.rejection().retryAfterMs() > 0, "move-budget must report a positive retry delay");
        require(before.equals(domain.state("player")), "Rejected move changed position, sequence or budget");
        now.addAndGet(rejected.rejection().retryAfterMs());
        var accepted = domain.move("player", new MoveRequest(Rules.MAX_STEP, 2));
        require(accepted.accepted() && accepted.position().x() == 120, "Server retry delay was insufficient");
    }

    private void sequenceRange() {
        var domain = create(new AtomicLong());
        requireRejected(domain, 1, 0, "stale-sequence", "player");
        requireRejected(domain, 1, Rules.MAX_SAFE_INTEGER + 1, "integer-out-of-range", "player");
        require(Rules.MAX_SAFE_INTEGER == 9_007_199_254_740_991L, "Contract integer limit changed");
        var gap = domain.move("player", new MoveRequest(1, 2));
        require(gap.accepted() && gap.sequence() == 2, "A newer sequence greater than the previous value was rejected");

        var maxDomain = new ZonePositionDomain("zone-2", () -> 0);
        maxDomain.addPlayer("last", new EnterZone(new Position(100), Rules.MAX_SAFE_INTEGER, 0));
        requireRejected(maxDomain, 1, Rules.MAX_SAFE_INTEGER, "stale-sequence", "last");
        requireRejected(maxDomain, 1, Long.MAX_VALUE, "integer-out-of-range", "last");

        var safeArrival = new ZonePositionDomain("zone-2", () -> 0);
        require(safeArrival.queueArrival("safe", new EnterZone(new Position(100), Rules.MAX_SAFE_INTEGER, 0)) == null,
                "Maximum safe sequence was rejected during admission");
        var unsafeArrival = new ZonePositionDomain("zone-2", () -> 0);
        var arrivalRejection = unsafeArrival.queueArrival(
                "unsafe", new EnterZone(new Position(100), Rules.MAX_SAFE_INTEGER + 1, 0));
        require(arrivalRejection != null && "integer-out-of-range".equals(arrivalRejection.reason())
                        && unsafeArrival.takeArrival("unsafe") == null,
                "Out-of-range admission sequence was accepted or stored");
    }

    private void initialLocation() {
        var valid = ZonePositionDomain.resolveLocation(299);
        require("zone-3".equals(valid.zoneId()) && valid.rejection() == null,
                "In-world initial position resolved to the wrong Zone");
        require("non-finite".equals(ZonePositionDomain.resolveLocation(Double.NaN).rejection().reason()),
                "NaN initial position was not rejected");
        require("non-finite".equals(ZonePositionDomain.resolveLocation(Double.POSITIVE_INFINITY).rejection().reason()),
                "Infinite initial position was not rejected");
        require("outside-world".equals(ZonePositionDomain.resolveLocation(600).rejection().reason()),
                "World-limit initial position was not rejected");
    }

    private void remainingTimeSnapshot() {
        var sourceNow = new AtomicLong();
        var source = create(sourceNow);
        require(source.move("player", new MoveRequest(Rules.MAX_STEP, 1)).accepted(),
                "Expected initial move to be accepted");
        sourceNow.addAndGet(75);
        var snapshot = source.capture();
        var destinationNow = new AtomicLong(500);
        var destination = new ZonePositionDomain("zone-2", destinationNow::get);
        destination.restore(snapshot);
        var rejected = destination.move("player", new MoveRequest(5, 2));
        require(rejected.rejection() != null, "Expected a budget rejection after relocation");
        require("move-budget".equals(rejected.rejection().reason()), "Relocation lost movement budget");
        require(rejected.rejection().retryAfterMs() == 25, "Relocation did not preserve remaining ms");
    }

    private void presenceLifecycle() {
        var domain = create(new AtomicLong());
        domain.addPlayer("observer", new EnterZone(new Position(101), 0, 0));
        require(domain.connectedPlayers().size() == 2, "Connected players were not tracked");
        var before = domain.state("player");
        var disconnected = domain.setConnected("player", false);
        require(disconnected != null && !disconnected.connected(), "Disconnect did not end visibility");
        require(domain.connectedPlayers().size() == 1, "Disconnected player remained visible");
        require(before.position().equals(domain.state("player").position()), "Disconnect erased position");
        var reconnected = domain.setConnected("player", true);
        require(reconnected != null && reconnected.connected(), "Reconnect did not restore visibility");
        require(before.position().equals(domain.state("player").position()), "Reconnect changed position");
        var left = domain.removePlayer("player");
        require(left != null && left.position().equals(before.position()), "Zone leave lost last position");
        require(domain.connectedPlayers().size() == 1, "Zone leave retained player");
    }

    private void jsonContract() throws Exception {
        var left = Json.MAPPER.writeValueAsString(new PlayerLeft("player", PlayerLeftReason.LEFT_ZONE));
        require(left.contains("\"reason\":\"left-zone\""), "PlayerLeft enum was not serialized by name");
        var border = Json.MAPPER.writeValueAsString(new BorderPresence(
                "zone-2", "player", new Position(10), 1, BorderPresenceKind.MOVED, null));
        require(border.contains("\"kind\":\"moved\""), "Border event kind was not serialized by name");
        require(border.contains("\"reason\":null"), "Nullable border reason was omitted");
        require(Json.MAPPER.readValue(border, BorderPresence.class).reason() == null,
                "Nullable border reason did not round-trip");
        var rejected = Json.MAPPER.writeValueAsString(new ActionRejected("outside-world", 0));
        require(rejected.contains("\"reason\":\"outside-world\""), "ActionRejected reason field changed");
        require(rejected.contains("\"retryAfterMs\":0"), "ActionRejected retryAfterMs field changed");
        require(ActionRejected.class.getRecordComponents()[1].getType() == int.class,
                "RetryAfterMs must be a 32-bit integer");
    }

    private void check(String name, CheckedCheck test) throws Exception {
        test.run();
        System.out.println("PASS " + name);
    }

    public static void run() throws Exception {
        var checks = new DomainChecks();
        checks.check("non-finite movement", checks::nonFiniteMovement);
        checks.check("step limit", checks::stepLimit);
        checks.check("world boundary", checks::worldBoundary);
        checks.check("movement budget", checks::movementBudget);
        checks.check("stale and unsafe sequence", checks::sequenceRange);
        checks.check("initial location resolution", checks::initialLocation);
        checks.check("remaining-time relocation snapshot", checks::remainingTimeSnapshot);
        checks.check("presence lifecycle", checks::presenceLifecycle);
        checks.check("enum and nullable JSON", checks::jsonContract);
        System.out.println("Domain checks: 9/9 passed; assertions: " + checks.assertions);
    }

    @FunctionalInterface
    private interface CheckedCheck {
        void run() throws Exception;
    }
}
