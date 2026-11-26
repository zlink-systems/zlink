package MyGame.table

import MyGame.*
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
    val balances: Map<String, Long> = emptyMap(),
)

class PrepareTable : ZLinkSuspendingSpotPacketHandler<Table, PrepareRoom> {
    override suspend fun handle(spot: Table, message: PrepareRoom) {
        spot.context
            .outbound()
            .kotlin()
            .sendToSpot(Names.POOL_KEY, spot.prepare(message))
            .instanceSpot(Names.POOL)
            .inMesh(Names.MESH)
            .await()
    }
}

class CommitTable : ZLinkSuspendingSpotPacketHandler<Table, CommitRoom> {
    override suspend fun handle(spot: Table, message: CommitRoom) = spot.commit(message)
}

class WithdrawTable : ZLinkSuspendingSpotPacketHandler<Table, WithdrawRoom> {
    override suspend fun handle(spot: Table, message: WithdrawRoom) {
        spot.withdraw(message)
    }
}
