package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerWinMilestoneEvent;

// --8<-- [start:doc-multicast-subscribe]
// --8<-- [start:doc-ttt-milestone-handler]
@ZLinkSpotSubscription(topic = SampleNames.PlayerMilestoneTopic)
public final class PlayerWinMilestoneEventHandler
    implements ZLinkSpotSubscriptionHandler<PlayEntrySpot, PlayerWinMilestoneEvent> {
    @Override
    public CompletionStage<Void> handle(
        PlayEntrySpot spot,
        PlayerWinMilestoneEvent event) {
        spot.notifyMilestone(event);
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:doc-ttt-milestone-handler]
// --8<-- [end:doc-multicast-subscribe]
