package MyGame;

public record MatchRequest(String playerId, String mode) {
    public MatchRequest {
        if (mode == null) mode = Names.DEFAULT_MODE;
    }

    public MatchRequest(String playerId) {
        this(playerId, Names.DEFAULT_MODE);
    }
}
