package MyGame.table

import MyGame.*
import org.springframework.stereotype.Component
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.*

interface PlayerData {
    suspend fun loadPlayer(playerId: String): Long

    suspend fun commitRound(tableId: String, handNumber: Long, balances: Map<String, Long>)
}

@Component
class PlayerDataChannel(private val routes: ZLinkRouteClient) : PlayerData {
    override suspend fun loadPlayer(playerId: String): Long {
        val record =
            routes
                .kotlin()
                .requestToChannel<PlayerRecord>(Names.PLAYER_DATA, LoadPlayer(playerId))
                .await()
        check(record.playerId == playerId)
        return record.chips
    }

    override suspend fun commitRound(
        tableId: String,
        handNumber: Long,
        balances: Map<String, Long>,
    ) {
        val request =
            CommitRound(tableId, handNumber, balances.map { PlayerRecord(it.key, it.value) })
        val reply =
            routes.kotlin().requestToChannel<RoundCommitted>(Names.PLAYER_DATA, request).await()
        check(reply == RoundCommitted(tableId, handNumber))
    }
}
