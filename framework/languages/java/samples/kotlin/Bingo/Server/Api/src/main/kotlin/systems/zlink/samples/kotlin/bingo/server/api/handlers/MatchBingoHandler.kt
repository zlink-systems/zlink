package systems.zlink.samples.kotlin.bingo.server.api.handlers

import kotlinx.coroutines.future.await
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoApiRes

@ZLinkHandlerGroup(SampleNames.ApiChannel)
class MatchBingoHandler(
    private val routes: ZLinkRouteClient,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingRequestHandler<MatchBingoApiReq, MatchBingoApiRes> {
    override suspend fun handle(
        request: MatchBingoApiReq,
        context: ZLinkMessageContext,
    ) = run {
        val levelBucket = "1-10"
        val allocated = routes
            .requestToSpot(
                "match:$levelBucket",
                ReserveBingoRoomReq(
                    request.actorId,
                    request.mode,
                    levelBucket,
                ),
            )
            .instanceSpot(SampleNames.MatchmakerSpotType)
            .inMesh(SampleNames.MatchmakingMesh)
            .timeout(SampleTimings.RequestTimeout)
            .submit(ReserveBingoRoomRes::class.java)
            .await()

        spots.getOrCreate(allocated.roomId, SampleNames.RoomSpotType)
            .inMesh(SampleNames.Mesh)
            .request(allocated.settings)
            .timeout(SampleTimings.RequestTimeout)
            .submit()
            .await()

        MatchBingoApiRes(allocated.roomId)
    }
}
