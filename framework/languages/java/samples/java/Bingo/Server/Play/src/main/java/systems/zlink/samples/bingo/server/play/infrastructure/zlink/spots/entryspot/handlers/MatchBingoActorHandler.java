package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.entryspot.handlers;
import java.util.concurrent.CompletionStage;
import java.util.List;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class MatchBingoActorHandler
    implements ZLinkEntrySpotActorRequestHandler<
        BingoEntrySpot,
        PlayerActor,
        Messages.MatchBingoReq,
        Messages.MatchBingoRes> {
    @Override
    public CompletionStage<Messages.MatchBingoRes> handle(
        BingoEntrySpot entrySpot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.MatchBingoReq request) {
        return entrySpot.context().outbound().requestToChannel(
                SampleNames.ApiChannel,
                BingoMessages.matchBingoApiReq(
                    actor.actorId(),
                    actor.displayName(),
                    request.getMode()))
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.MatchBingoApiRes.class)
            .thenApply(matched -> {
                actor.trackDeferredJoin(matched.getRoomId());
                actor.context().joinSpot(
                    matched.getRoomId(),
                    BingoMessages.bingoRoomJoinReq(
                        matched.getRoomId(),
                        actor.actorId(),
                        actor.displayName(),
                        false))
                    .timeout(SampleTimings.RequestTimeout)
                    .defer();
                Messages.BingoRoomState initialState = BingoMessages.bingoRoomState(
                    matched.getRoomId(),
                    "WaitingForPlayers",
                    actor.actorId(),
                    false,
                    0,
                    null,
                    List.of(),
                    List.of(BingoMessages.bingoPlayerState(
                        actor.actorId(),
                        actor.displayName(),
                        1,
                        true,
                        List.of(),
                        List.of(),
                        0,
                        0,
                        0)),
                    List.of());
                return BingoMessages.matchBingoRes(matched.getRoomId(), initialState);
            });
    }
}
