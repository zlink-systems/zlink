package systems.zlink.tutorial.shared

// Message contracts shared by both processes. They are plain data classes: the
// Framework serializes them, and nothing here is registered or annotated.

// --8<-- [start:channel-contracts]
data class GetPlayerProfile(val playerId: String)

data class PlayerProfile(val playerId: String, val nickname: String, val level: Int)

// One-way: the caller does not wait, so this message has no reply class.
data class RecordLogin(val playerId: String)

// --8<-- [end:channel-contracts]

// --8<-- [start:clientserver-contracts]
data class IssueSessionTicket(val playerId: String)

data class SessionTicket(val value: String)

// --8<-- [end:clientserver-contracts]

// --8<-- [start:fanout-contracts]
// Published without naming a recipient. Every subscribed node receives it.
data class MaintenanceNotice(val message: String)

// --8<-- [end:fanout-contracts]

// --8<-- [start:spot-contracts]
// Reaches the room's create callback rather than a handler, so it carries what
// the room needs in order to exist.
data class OpenRoom(val title: String)

// One-way: the caller does not wait for the room to record the line.
data class PostChat(val playerId: String, val text: String)

class GetRoomState

data class RoomState(val title: String, val chat: List<String>)

// --8<-- [end:spot-contracts]

// --8<-- [start:instance-spot-contracts]
// A match queue has no create call, so nothing here corresponds to OpenRoom.
data class JoinMatchQueue(val playerId: String)

data class MatchQueueStatus(val waiting: Int)

// --8<-- [end:instance-spot-contracts]

// --8<-- [start:actor-contracts]
// Reaches the player's create callback rather than a handler.
data class CreatePlayer(val nickname: String)

// One-way: the caller does not wait for the rename to be recorded.
data class ChangeNickname(val nickname: String)

class GetPlayer

data class PlayerInfo(val playerId: String, val nickname: String)

// --8<-- [end:actor-contracts]

// --8<-- [start:stream-contracts]
// Exchanged over the external TCP connection, not between mesh nodes. The
// stream codec wants 64-bit integers as decimal strings, so the timestamp is
// carried as text rather than as a Long.
data class Ping(val sentAtUnixMs: String)

data class Pong(val sentAtUnixMs: String)

// --8<-- [end:stream-contracts]

// --8<-- [start:session-actor-contracts]
data class Authenticate(val playerId: String)

data class Authenticated(val playerId: String)

// Pushed by the player to its own connection, with no request to answer.
data class NicknameChanged(val nickname: String)

// --8<-- [end:session-actor-contracts]

// --8<-- [start:node-direct-contracts]
// Answered by the node itself rather than by a channel, so the reply describes
// that one process.
class GetNodeStatus

data class NodeStatus(
    val meshName: String,
    val channelName: String,
    val calledBy: String,
    val uptime: String,
    // Int, not Long. The framework-json codec carries 64-bit integers as decimal
    // strings, so a Long here would reach the caller quoted.
    val processId: Int,
)
// --8<-- [end:node-direct-contracts]
