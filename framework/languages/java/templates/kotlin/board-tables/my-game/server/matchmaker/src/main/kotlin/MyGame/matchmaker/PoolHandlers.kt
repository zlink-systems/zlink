package MyGame.matchmaker

import MyGame.*
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.ZLinkTimerTick

@ZLinkHandlerGroup(Names.POOL)
class PoolRequests {
    @ZLinkSpotRequest
    suspend fun request(pool: Pool, request: MatchRequest): MatchQueued = pool.enqueue(request)

    @ZLinkSpotRequest
    suspend fun cancel(pool: Pool, request: CancelTicket): MatchCancelled = pool.cancel(request)
}

class EnterHandler : ZLinkSuspendingSpotPacketHandler<Pool, EnterReport> {
    override suspend fun handle(spot: Pool, message: EnterReport) = spot.entered(message)
}

class DisconnectHandler : ZLinkSuspendingSpotPacketHandler<Pool, TicketDisconnected> {
    override suspend fun handle(spot: Pool, message: TicketDisconnected) =
        spot.disconnected(message)
}

class MatchTick : ZLinkSuspendingSpotTimerHandler<Pool> {
    override suspend fun handle(spot: Pool, tick: ZLinkTimerTick) = spot.tick()
}
