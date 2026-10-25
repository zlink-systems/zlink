package MyGame;

public record PartyCommand(String actorId, boolean join, boolean enter) {
    public PartyCommand(String actorId) { this(actorId, false, false); }
}
