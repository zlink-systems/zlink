package MyGame

object LobbyNames {
    const val MATCH_PATH = "/match"
}

object HttpProtocol {
    const val POST = "POST"
    const val CONTENT_TYPE = "Content-Type"
    const val JSON = "application/json"
    const val OK = 200
    const val BAD_REQUEST = 400
    const val NOT_FOUND = 404
    const val UNAVAILABLE = 503
}

object Roles {
    const val MATCH = "match"
    const val SESSION = "session"
    const val LOBBY = "http-lobby"
    const val EXTERNAL = "external"
    const val LOCATION = "location-store"
}

data class MatchRequest(val playerId: String = "", val mode: String = Names.DEFAULT_MODE)

data class MatchResponse(val roomId: String = "", val sessionEndpoint: String = "")

data class JoinRoomRequest(val playerId: String = "", val roomId: String = "")

data class PrepareRoom(val matchId: String = "", val members: List<String> = emptyList())

data class RoomPrepared(val matchId: String = "")

data class HttpNodeReady(
    val id: String = "",
    val meshEndpoint: String = "",
    val streamEndpoint: String? = null,
    val httpEndpoint: String? = null,
)
