package MyGame.matchmaker;

import java.util.List;
import java.util.stream.Stream;

final class MatchRule {
    private static final int MEMBER_COUNT = 2;

    List<MatchQueue.Ticket> choose(Stream<MatchQueue.Ticket> waiting) {
        var members = waiting.limit(MEMBER_COUNT).toList();
        return members.size() == MEMBER_COUNT ? members : List.of();
    }
}
