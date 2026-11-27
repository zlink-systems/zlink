package MyGame

data class LoadPlayer(val playerId: String)

data class PlayerRecord(val playerId: String, val chips: Long)

data class CommitRound(val tableId: String, val handNumber: Long, val results: List<PlayerRecord>)

data class RoundCommitted(val tableId: String, val handNumber: Long)
