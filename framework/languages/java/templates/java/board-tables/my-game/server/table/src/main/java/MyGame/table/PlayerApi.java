package MyGame.table;

import java.util.Map;
import java.util.concurrent.CompletionStage;

public interface PlayerApi {
    CompletionStage<Long> loadPlayer(String playerId);

    CompletionStage<Void> commitRound(String tableId, long handNumber, Map<String, Long> balances);
}
