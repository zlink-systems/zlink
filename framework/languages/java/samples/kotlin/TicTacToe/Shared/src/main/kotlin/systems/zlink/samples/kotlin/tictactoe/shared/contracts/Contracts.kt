package systems.zlink.samples.kotlin.tictactoe.shared.contracts

data class AuthenticatePlayerReq(val accessToken: String)

data class AuthenticatePlayerRes(val player: PlayerInfo)

data class AuthenticateRes(val player: PlayerInfo)

data class AuthenticateReq(val accessToken: String)

data class CreateGameHttpReq(val gameName: String?)

data class CreateGameHttpRes(
    val roomId: String,
    val gameName: String,
    val playEndpoints: List<String>,
    val playNodes: List<PlayNodeInfo>,
    val requiredLevel: Int,
)

data class PlayNodeInfo(
    val streamEndpoint: String,
)

data class PlayerInfo(
    val actorId: String,
    val displayName: String,
    val level: Int,
    val wins: Int,
)

data class GameState(
    val roomId: String,
    val board: String,
    val status: String,
    val winner: String?,
    val nextTurn: String,
    val xActorId: String?,
    val oActorId: String?,
    val lastMoveActorId: String?,
    val lastMoveCell: Int?,
)

data class GameStateNotify(val state: GameState)

data class JoinGameRes(val state: GameState)

data class JoinGameReq(val roomId: String)

data class JoinGameFailedNotify(
    val roomId: String,
    val reason: String,
    val retriable: Boolean,
)

data class TicTacToeGameJoinReq(val roomId: String, val player: PlayerInfo)

data class TicTacToeGameJoinRes(val state: GameState)

data class PlaceMarkRes(val state: GameState)

data class PlaceMarkReq(val cell: Int)

data class PlayerJoinedNotify(
    val roomId: String,
    val actorId: String,
    val displayName: String,
    val level: Int,
    val mark: String,
    val state: GameState,
)

class ObserveMilestoneReq

data class ObserveMilestoneRes(val subscribed: Boolean)

data class LeaveGameReq(val roomId: String)

data class PlayerWinMilestoneMsg(
    val roomId: String,
    val actorId: String,
    val displayName: String,
    val wins: Int,
)

data class WinMilestoneNotify(
    val roomId: String,
    val actorId: String,
    val displayName: String,
    val wins: Int,
)
