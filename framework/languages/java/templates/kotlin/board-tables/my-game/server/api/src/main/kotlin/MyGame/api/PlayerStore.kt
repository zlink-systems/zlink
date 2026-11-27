package MyGame.api

import MyGame.*
import io.lettuce.core.RedisClient
import io.lettuce.core.SetArgs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.springframework.stereotype.Component
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler

@Component
class PlayerStore(config: NodeConfig, private val redis: RedisClient) {
    private val prefix = config.keyPrefix + "players:"

    suspend fun loadPlayer(playerId: String): Long =
        withContext(Dispatchers.IO) {
            redis.connect().use { connection ->
                val stored =
                    connection
                        .sync()
                        .setGet(
                            balance(playerId),
                            Rules.INITIAL_CHIPS.toString(),
                            SetArgs.Builder.nx(),
                        )
                stored?.toLong() ?: Rules.INITIAL_CHIPS
            }
        }

    suspend fun commitRound(round: CommitRound) {
        withContext(Dispatchers.IO) {
            redis.connect().use { connection ->
                val commands = connection.sync()
                val marker = "${prefix}round:${round.tableId}:${round.handNumber}"
                commands.watch(marker)
                if (commands.exists(marker) != 0L) return@use

                val balances =
                    round.results.associate { balance(it.playerId) to it.chips.toString() }
                commands.multi()
                commands.mset(balances + (marker to "1"))
                val result = commands.exec()
                if (result.wasDiscarded()) {
                    // Only a commit for this round can change the watched marker.
                    check(commands.exists(marker) == 1L) { "Round transaction discarded: $marker" }
                } else {
                    check(result.get<String>(0) == "OK") { "Round transaction failed: $marker" }
                }
            }
        }
    }

    private fun balance(playerId: String): String = "${prefix}by-id:$playerId"
}

@ZLinkHandlerGroup(Names.PLAYER_DATA)
class LoadPlayerHandler(private val players: PlayerStore) :
    ZLinkSuspendingRequestHandler<LoadPlayer, PlayerRecord> {
    override suspend fun handle(request: LoadPlayer, context: ZLinkMessageContext): PlayerRecord =
        PlayerRecord(request.playerId, players.loadPlayer(request.playerId))
}

@ZLinkHandlerGroup(Names.PLAYER_DATA)
class CommitRoundHandler(private val players: PlayerStore) :
    ZLinkSuspendingRequestHandler<CommitRound, RoundCommitted> {
    override suspend fun handle(
        request: CommitRound,
        context: ZLinkMessageContext,
    ): RoundCommitted {
        players.commitRound(request)
        return RoundCommitted(request.tableId, request.handNumber)
    }
}
