package MyGame;

import java.util.List;

public record RevealedHand(int seat, String playerId, List<Integer> cards) {}
