package systems.zlink.samples.kotlin.bingo.server.api.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomCreateReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomRes

@ZLinkHandlerGroup(SampleNames.ApiChannel)
class MatchBingoHandler(private val routes: ZLinkRouteClient, private val spots: ZLinkSpotManager) :
    ZLinkSuspendingRequestHandler<MatchBingoApiReq, MatchBingoApiRes> {
    private val kotlinRoutes = routes.kotlin()
    private val kotlinSpots = spots.kotlin()

    override suspend fun handle(request: MatchBingoApiReq, context: ZLinkMessageContext) = run {
        // --8<-- [start:doc-bingo-api-match]
        val levelBucket = "1-10"
        val allocated =
            kotlinRoutes
                .requestToSpot<ReserveBingoRoomRes>(
                    "match:$levelBucket",
                    ReserveBingoRoomReq(request.actorId, request.mode, levelBucket),
                )
                .instanceSpot(SampleNames.MatchmakerSpotType)
                .inMesh(SampleNames.MatchmakingMesh)
                .timeout(SampleTimings.RequestTimeout)
                .await()

        kotlinSpots
            .getOrCreate(allocated.roomId, SampleNames.RoomSpotType)
            .inMesh(SampleNames.Mesh)
            .request(BingoRoomCreateReq(allocated.settings))
            .timeout(SampleTimings.RequestTimeout)
            .await()
        // --8<-- [end:doc-bingo-api-match]

        MatchBingoApiRes(allocated.roomId)
    }
}
