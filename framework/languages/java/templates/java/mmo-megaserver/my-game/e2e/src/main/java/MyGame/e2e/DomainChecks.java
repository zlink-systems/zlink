package MyGame.e2e;

import MyGame.*;
import MyGame.region.domain.*;
import java.util.*;
import java.util.function.LongSupplier;

public final class DomainChecks {
    private static final class Clock implements LongSupplier {
        long now;
        public long getAsLong() { return now; }
        void advance(long ms) { now += ms * 1_000_000L; }
    }
    private static int count;
    private static void check(boolean ok, String name) {
        if (!ok) throw new IllegalStateException("Domain boundary failed: " + name);
        count++;
    }
    private static boolean rejected(Positions p, String id, double delta, long sequence, String reason, int retry) {
        var before = p.snapshot();
        return new ActionRejected(reason, retry).equals(p.move(id, delta, sequence)) && before.equals(p.snapshot());
    }
    public static void run() throws Exception {
        count = 0;
        check(Names.MAX_SPEED == Names.MAX_STEP * 5.0, "movement speed contract");
        var clock = new Clock();
        var p = new Positions(clock);
        p.admit("p1", 2);
        check(p.move("p1", Names.MAX_STEP, 1) == null && p.view().equals(List.of(new PlayerPosition("p1", Names.MAX_STEP, 1))), "first movement accepts MaxStep");
        check(rejected(p, "p1", Names.MAX_STEP, 2, "move-budget", 200), "budget rejection preserves state");
        var before = p.snapshot();
        var fractional = p.move("p1", 0.01, 2);
        check(fractional != null && fractional.reason().equals("move-budget") && fractional.retryAfterMs() > 0 && before.equals(p.snapshot()), "fractional budget retry");
        check(rejected(p, "p1", Names.MAX_STEP + 1, 2, "step-too-large", 0), "step maximum plus one");
        check(rejected(p, "p1", 1, 1, "stale-sequence", 0), "current sequence stale");
        for (var value : new double[] {Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY})
            check(rejected(p, "p1", value, 2, "non-finite", 0), "non-finite domain input");
        var edge = new Positions(clock);
        edge.restore(Map.of("edge", new Position(Names.REGION_MAX_X, 0, 0)));
        check(rejected(edge, "edge", 1, 1, "outside-region", 0), "maximum plus one");
        edge.restore(Map.of("edge", new Position(Names.REGION_MIN_X, 0, 0)));
        check(rejected(edge, "edge", -1, 1, "outside-region", 0), "minimum minus one");
        var safe = new Positions(clock);
        safe.admit("safe", 2);
        check(safe.move("safe", 0, Names.MAX_SAFE_INTEGER) == null, "maximum sequence");
        check(rejected(safe, "safe", 0, Names.MAX_SAFE_INTEGER + 1, "integer-out-of-range", 0), "maximum sequence plus one");
        var oversized = new Positions(clock);
        oversized.admit("large", 2);
        check(rejected(oversized, "large", Double.MAX_VALUE, 1, "step-too-large", 0), "finite oversized delta");
        var source = new Positions(new Clock());
        source.admit("relocated", 2);
        check(source.move("relocated", Names.MAX_STEP, 1) == null, "snapshot source move");
        var snapshot = source.snapshot();
        check(snapshot.get("relocated").remainingMs() == 200, "remaining milliseconds");
        check(snapshot.equals(source.snapshot()), "capture preserves source");
        var targetClock = new Clock();
        targetClock.now = 8L * 3600 * 1_000_000_000L;
        var target = new Positions(targetClock);
        var wire = Json.MAPPER.writeValueAsBytes(snapshot);
        target.restore(Json.MAPPER.readValue(wire, new com.fasterxml.jackson.core.type.TypeReference<LinkedHashMap<String, Position>>() {}));
        check(source.view().equals(target.view()) && target.snapshot().get("relocated").remainingMs() == 200, "different clock origin restore");
        check(rejected(target, "relocated", Names.MAX_STEP, 2, "move-budget", 200), "restored budget retained");
        targetClock.advance(200);
        check(target.move("relocated", Names.MAX_STEP, 2) == null, "server retry boundary");
        System.out.println("Domain boundaries: " + count + "/20 passed");
    }
}
