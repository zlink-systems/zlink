package systems.zlink.tutorial.shared;

// Message contracts shared by both processes. They are plain records: the
// Framework serializes them, and nothing here is registered or annotated.
//
// Java allows one public type per file, so the records the .NET tutorial keeps
// side by side in Contracts.cs are nested in one container class here. The
// Framework derives a packet name from the simple name, so a nested record
// carries the same packet name a top-level one would.
public final class Contracts {

    private Contracts() {}

    // --8<-- [start:channel-contracts]
    public record GetPlayerProfile(String playerId) {}

    public record PlayerProfile(String playerId, String nickname, int level) {}

    // One-way: the caller does not wait, so this message has no reply record.
    public record RecordLogin(String playerId) {}

    // --8<-- [end:channel-contracts]

    // --8<-- [start:clientserver-contracts]
    public record IssueSessionTicket(String playerId) {}

    public record SessionTicket(String value) {}

    // --8<-- [end:clientserver-contracts]

    // --8<-- [start:fanout-contracts]
    // Published without naming a recipient. Every subscribed node receives it.
    public record MaintenanceNotice(String message) {}

    // --8<-- [end:fanout-contracts]

    // --8<-- [start:spot-contracts]
    // Reaches the room's create callback rather than a handler, so it carries
    // what the room needs in order to exist.
    public record OpenRoom(String title) {}

    // One-way: the caller does not wait for the room to record the line.
    public record PostChat(String playerId, String text) {}

    public record GetRoomState() {}

    public record RoomState(String title, java.util.List<String> chat) {}

    // --8<-- [end:spot-contracts]

    // --8<-- [start:instance-spot-contracts]
    // A match queue has no create call, so nothing here corresponds to OpenRoom.
    public record JoinMatchQueue(String playerId) {}

    public record MatchQueueStatus(int waiting) {}

    // --8<-- [end:instance-spot-contracts]

    // --8<-- [start:actor-contracts]
    // Reaches the player's create callback rather than a handler.
    public record CreatePlayer(String nickname) {}

    // One-way: the caller does not wait for the rename to be recorded.
    public record ChangeNickname(String nickname) {}

    public record GetPlayer() {}

    public record PlayerInfo(String playerId, String nickname) {}

    // --8<-- [end:actor-contracts]

    // --8<-- [start:stream-contracts]
    // Exchanged over the external TCP connection, not between mesh nodes. The
    // stream codec wants 64-bit integers as decimal strings, so the timestamp is
    // carried as text rather than as a long.
    public record Ping(String sentAtUnixMs) {}

    public record Pong(String sentAtUnixMs) {}

    // --8<-- [end:stream-contracts]

    // --8<-- [start:session-actor-contracts]
    public record Authenticate(String playerId) {}

    public record Authenticated(String playerId) {}

    // Pushed by the player to its own connection, with no request to answer.
    public record NicknameChanged(String nickname) {}

    // --8<-- [end:session-actor-contracts]

    // --8<-- [start:node-direct-contracts]
    // Answered by the node itself rather than by a channel, so the reply describes
    // that one process.
    public record GetNodeStatus() {}

    public record NodeStatus(
            String meshName, String channelName, String calledBy, String uptime, int processId) {}
    // --8<-- [end:node-direct-contracts]
}
