package systems.zlink.samples.zoneworld.shared;

import java.nio.charset.StandardCharsets;
import java.util.Comparator;
import java.util.List;

public final class ZoneWorldSpec {
    public static final int WORLD_SIZE = 100;
    public static final int ZONE_SPLIT = 50;
    public static final int BORDER_BAND = 10;
    public static final int MAX_STEP_PER_AXIS = 5;
    public static final int SPAWN_X = 25;
    public static final int SPAWN_Y = 25;
    public static final int TICK_PERIOD_MS = 100;
    public static final int BOT_TICK_PERIOD_MS = 500;
    public static final int BOT_STEP = 3;
    public static final int BORDER_EXPIRY_TICKS = 3;
    public static final Comparator<String> UTF8_ORDER = ZoneWorldSpec::compareUtf8;

    private ZoneWorldSpec() {
    }

    public static String zoneOf(int x, int y) {
        if (x < ZONE_SPLIT && y < ZONE_SPLIT) return "zone-nw";
        if (x >= ZONE_SPLIT && y < ZONE_SPLIT) return "zone-ne";
        if (x < ZONE_SPLIT) return "zone-sw";
        return "zone-se";
    }

    public static boolean inRange(int x, int y) {
        return x >= 0 && x < WORLD_SIZE && y >= 0 && y < WORLD_SIZE;
    }

    public static boolean isWest(String zone) {
        return zone.equals("zone-nw") || zone.equals("zone-sw");
    }

    public static boolean isNorth(String zone) {
        return zone.equals("zone-nw") || zone.equals("zone-ne");
    }

    public static List<String> adjacentZones(String zone) {
        return switch (zone) {
            case "zone-nw" -> List.of("zone-ne", "zone-sw");
            case "zone-ne" -> List.of("zone-nw", "zone-se");
            case "zone-sw" -> List.of("zone-nw", "zone-se");
            case "zone-se" -> List.of("zone-ne", "zone-sw");
            default -> throw new IllegalArgumentException("unknown zone: " + zone);
        };
    }

    public static boolean inBorderBand(int x, int y, String fromZone, String toZone) {
        if (!zoneOf(x, y).equals(fromZone)) return false;
        boolean crossesX = isWest(fromZone) != isWest(toZone);
        boolean crossesY = isNorth(fromZone) != isNorth(toZone);
        if (crossesX == crossesY) return false;
        int distance;
        if (crossesX) {
            distance = isWest(fromZone) ? ZONE_SPLIT - 1 - x : x - ZONE_SPLIT;
        } else {
            distance = isNorth(fromZone) ? ZONE_SPLIT - 1 - y : y - ZONE_SPLIT;
        }
        return Math.abs(distance) < BORDER_BAND;
    }

    public static String nodeOf(String zone) {
        return switch (zone) {
            case "zone-nw", "zone-sw" -> "zone-node-1";
            case "zone-ne", "zone-se" -> "zone-node-2";
            default -> throw new IllegalArgumentException("unknown zone: " + zone);
        };
    }

    public static List<String> zonesOf(String nodeId) {
        return switch (nodeId) {
            case "zone-node-1" -> List.of("zone-nw", "zone-sw");
            case "zone-node-2" -> List.of("zone-ne", "zone-se");
            default -> List.of();
        };
    }

    public static MoveDecision validateMove(int fromX, int fromY, int toX, int toY) {
        if (!inRange(toX, toY)) return new MoveDecision(false, false, "OutOfRange");
        if (Math.abs(toX - fromX) > MAX_STEP_PER_AXIS
            || Math.abs(toY - fromY) > MAX_STEP_PER_AXIS) {
            return new MoveDecision(false, false, "TooFar");
        }
        String from = zoneOf(fromX, fromY);
        String to = zoneOf(toX, toY);
        boolean crossesX = isWest(from) != isWest(to);
        boolean crossesY = isNorth(from) != isNorth(to);
        if (crossesX && crossesY) return new MoveDecision(false, false, "DiagonalCrossing");
        return new MoveDecision(true, !from.equals(to), null);
    }

    public static int compareUtf8(String left, String right) {
        byte[] a = left.getBytes(StandardCharsets.UTF_8);
        byte[] b = right.getBytes(StandardCharsets.UTF_8);
        int length = Math.min(a.length, b.length);
        for (int i = 0; i < length; i++) {
            int difference = Byte.toUnsignedInt(a[i]) - Byte.toUnsignedInt(b[i]);
            if (difference != 0) return difference;
        }
        return Integer.compare(a.length, b.length);
    }

    public record MoveDecision(boolean accepted, boolean zoneChanged, String reason) {
    }

    public record BotFixture(String id, int x, int y, int dirX, int dirY) {
    }

    public static List<BotFixture> bots() {
        return List.of(
            new BotFixture("bot-nw-x", 10, 15, 1, 0),
            new BotFixture("bot-nw-y", 15, 10, 0, 1),
            new BotFixture("bot-ne-x", 90, 15, -1, 0),
            new BotFixture("bot-ne-y", 85, 10, 0, 1),
            new BotFixture("bot-sw-x", 10, 85, 1, 0),
            new BotFixture("bot-sw-y", 15, 90, 0, -1),
            new BotFixture("bot-se-x", 90, 85, -1, 0),
            new BotFixture("bot-se-y", 85, 90, 0, -1));
    }
}
