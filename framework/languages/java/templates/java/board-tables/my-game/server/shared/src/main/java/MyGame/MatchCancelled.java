package MyGame;

public record MatchCancelled(String ticketId, String reason) {
    public static final String CANCELLED = "cancelled";
    public static final String EXPIRED = "expired";
    public static final String DISCONNECTED = "disconnected";
}
