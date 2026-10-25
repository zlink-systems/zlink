package MyGame;

import java.util.ArrayList;
import java.util.List;

public final class Rules {
    private Rules() {}

    public static final int ZONE_WIDTH = 100, ZONE_COUNT = 6, BORDER_RADIUS = 10;
    public static final long MAX_SAFE_INTEGER = 9_007_199_254_740_991L;
    public static final double MAX_STEP = 10, MAX_SPEED = MAX_STEP * 5;
    public static final int FULL_BUDGET_AFTER_MS = 200;
    public static final String ZONE_PREFIX = "zone-";

    public static String zoneId(double x) {
        return ZONE_PREFIX + ((int) Math.floor(x / ZONE_WIDTH) + 1);
    }

    public static List<String> adjacentZones(String id) {
        int number = Integer.parseInt(id.substring(ZONE_PREFIX.length()));
        if (number < 1 || number > ZONE_COUNT) throw new IllegalArgumentException("Unknown Zone");
        var adjacent = new ArrayList<String>(2);
        if (number > 1) adjacent.add(ZONE_PREFIX + (number - 1));
        if (number < ZONE_COUNT) adjacent.add(ZONE_PREFIX + (number + 1));
        return List.copyOf(adjacent);
    }

    public static String borderTopic(String source, String target) {
        return source + ":" + target;
    }
}
