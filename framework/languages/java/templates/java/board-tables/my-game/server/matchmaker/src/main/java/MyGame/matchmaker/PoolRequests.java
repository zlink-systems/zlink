package MyGame.matchmaker;

import MyGame.*;

import systems.zlink.framework.handlers.*;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.POOL)
public final class PoolRequests {
    @ZLinkSpotRequest
    public CompletionStage<MatchQueued> request(Pool pool, MatchRequest request) {
        return pool.request(request);
    }

    @ZLinkSpotRequest
    public CompletionStage<MatchCancelled> cancel(Pool pool, CancelTicket request) {
        return pool.cancel(request);
    }
}
