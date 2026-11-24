package MyGame;

import java.util.regex.Pattern;

public final class GameLog {
    private static final Pattern RUN_ID =
            Pattern.compile("([0-9a-f]{8})-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    private static final Pattern CONTROL_CHARACTERS = Pattern.compile("[\\p{Cc}\\p{Zl}\\p{Zp}]");
    private static final Pattern PLAY_NODE = Pattern.compile("play([A-Z])");

    private GameLog() {}

    public static String shortId(String id) {
        return CONTROL_CHARACTERS.matcher(RUN_ID.matcher(id).replaceAll("$1")).replaceAll("?");
    }

    public static String nodeTitle(String id) {
        var play = PLAY_NODE.matcher(id);
        if (play.matches()) return "Play node " + play.group(1);
        return switch (id) {
            case "session" -> "Session";
            case "matchmaker" -> "Matchmaker";
            default -> shortId(id);
        };
    }

    public static void table(String id, String message) {
        System.out.println("Table " + shortId(id) + ": " + message);
    }
}
