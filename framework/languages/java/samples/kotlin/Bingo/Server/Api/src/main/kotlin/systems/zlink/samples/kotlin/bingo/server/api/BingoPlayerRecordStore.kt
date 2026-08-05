package systems.zlink.samples.kotlin.bingo.server.api

import java.util.concurrent.ConcurrentHashMap
import org.springframework.stereotype.Component

@Component
class BingoPlayerRecordStore {
    private val records = ConcurrentHashMap<String, PlayerRecord>()

    fun get(actorId: String): PlayerRecord = records[actorId] ?: PlayerRecord(actorId, 0, 0)

    fun report(actorId: String, won: Boolean): PlayerRecord =
        records.compute(actorId) { _, current ->
            val record = current ?: PlayerRecord(actorId, 0, 0)
            if (won) record.copy(wins = record.wins + 1)
            else record.copy(losses = record.losses + 1)
        }!!

    data class PlayerRecord(val actorId: String, val wins: Int, val losses: Int)
}
