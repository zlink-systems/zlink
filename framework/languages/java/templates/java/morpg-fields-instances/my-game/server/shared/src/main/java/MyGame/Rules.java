package MyGame;

import java.util.ArrayList;
import java.util.List;

public final class Rules {
    private Rules() {}

    public static final int FIELD_WIDTH = 100, FIELD_COUNT = 4, BORDER_RADIUS = 10;
    public static final int MAXIMUM_PARTY_MEMBERS = 4, FINAL_STAGE = 3;
    public static final long MAX_SAFE_INTEGER = 9_007_199_254_740_991L;
    public static final double MAX_STEP = 10, MAX_SPEED = MAX_STEP * 5;
    public static final int FULL_BUDGET_MS = 200;

    public static String dungeonId(String runId) {
        return "dungeon-" + runId;
    }

    public static final long TICK_MILLIS = 100;
    public static final String FIELD_PREFIX = "field-";

    public static String fieldId(double x) {
        if (!Double.isFinite(x) || x < 0 || x >= FIELD_WIDTH * FIELD_COUNT)
            throw new IllegalArgumentException("Position outside world");
        return FIELD_PREFIX + ((int) (x / FIELD_WIDTH) + 1);
    }

    public static List<String> adjacentFields(String id) {
        int number = Integer.parseInt(id.substring(FIELD_PREFIX.length()));
        if (number < 1 || number > FIELD_COUNT) throw new IllegalArgumentException("Unknown Field");
        var adjacent = new ArrayList<String>(2);
        if (number > 1) adjacent.add(FIELD_PREFIX + (number - 1));
        if (number < FIELD_COUNT) adjacent.add(FIELD_PREFIX + (number + 1));
        return List.copyOf(adjacent);
    }

    public static String borderTopic(String source, String target) {
        return source + ":" + target;
    }
}
