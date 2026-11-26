package MyGame.table;

import MyGame.*;

import systems.zlink.framework.channels.ZLinkRouteClient;

import java.util.Map;
import java.util.concurrent.CompletionStage;

final class ChannelPlayerApi implements PlayerApi {
    private final ZLinkRouteClient client;

    ChannelPlayerApi(ZLinkRouteClient client) {
        this.client = client;
    }

    @Override
    public CompletionStage<Long> loadPlayer(String playerId) {
        return client.requestToChannel(Names.API, new LoadPlayer(playerId))
                .submit(PlayerRecord.class)
                .thenApply(PlayerRecord::chips);
    }

    @Override
    public CompletionStage<Void> commitRound(
            String tableId, long handNumber, Map<String, Long> balances) {
        var results =
                balances.entrySet().stream()
                        .map(entry -> new PlayerRecord(entry.getKey(), entry.getValue()))
                        .toList();
        return client.requestToChannel(Names.API, new CommitRound(tableId, handNumber, results))
                .submit(RoundCommitted.class)
                .thenApply(ignored -> null);
    }
}
