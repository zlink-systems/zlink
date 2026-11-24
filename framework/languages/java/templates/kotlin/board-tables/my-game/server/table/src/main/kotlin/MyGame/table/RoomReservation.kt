package MyGame.table

import MyGame.*
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

enum class RoomPhase {
    PREPARED,
    COMMITTED,
    WITHDRAWN,
}

data class RoomReservation(
    val assignmentId: String,
    val members: List<String>,
    val phase: RoomPhase = RoomPhase.PREPARED,
)

@ZLinkHandlerGroup(Names.TABLE)
class PrepareTable {
    @ZLinkSpotRequest
    suspend fun prepare(table: Table, request: PrepareRoom): RoomPrepared = table.prepare(request)
}

class CommitTable : ZLinkSuspendingSpotPacketHandler<Table, CommitRoom> {
    override suspend fun handle(spot: Table, message: CommitRoom) = spot.commit(message)
}

class WithdrawTable : ZLinkSuspendingSpotPacketHandler<Table, WithdrawRoom> {
    override suspend fun handle(spot: Table, message: WithdrawRoom) {
        spot.withdraw(message)
    }
}
