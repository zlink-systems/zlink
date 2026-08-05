package systems.zlink.samples.bingo.server.api.handlers;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.ApiChannel)
public final class MatchBingoHandler
    implements ZLinkRequestHandler<
        Messages.MatchBingoApiReq,
        Messages.MatchBingoApiRes> {
    private final ZLinkRouteClient routes;
    private final ZLinkSpotManager spots;

    public MatchBingoHandler(ZLinkRouteClient routes, ZLinkSpotManager spots) {
        this.routes = routes;
        this.spots = spots;
    }

    @Override
    public java.util.concurrent.CompletionStage<Messages.MatchBingoApiRes> handle(
        Messages.MatchBingoApiReq request,
        ZLinkMessageContext context) {
        String levelBucket = "1-10";
        return routes.requestToSpot(
                "match:" + levelBucket,
                BingoMessages.reserveBingoRoomReq(
                    request.getMode(), request.getActorId(), levelBucket))
            .instanceSpot(SampleNames.MatchmakerSpotType)
            .inMesh(SampleNames.MatchmakingMesh)
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.ReserveBingoRoomRes.class)
            .thenCompose(allocated -> spots
                .getOrCreate(allocated.getRoomId(), SampleNames.RoomSpotType)
                .inMesh(SampleNames.Mesh)
                .request(allocated.getSettings())
                .timeout(SampleTimings.RequestTimeout)
                .submit()
                .thenApply(ignored ->
                    BingoMessages.matchBingoApiRes(allocated.getRoomId())));
    }
}
