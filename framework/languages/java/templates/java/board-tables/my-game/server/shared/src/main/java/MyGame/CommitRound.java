package MyGame;

import java.util.List;

public record CommitRound(String tableId, long handNumber, List<PlayerRecord> results) {}
