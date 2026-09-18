namespace Tutorial.Shared;

// Message contracts shared by both processes. They are plain records: the
// Framework serializes them, and nothing here is registered or annotated.

// --8<-- [start:channel-contracts]
public sealed record GetPlayerProfile(string PlayerId);

public sealed record PlayerProfile(string PlayerId, string Nickname, int Level);

// One-way: the caller does not wait, so this message has no reply record.
public sealed record RecordLogin(string PlayerId);
// --8<-- [end:channel-contracts]

// --8<-- [start:clientserver-contracts]
public sealed record IssueSessionTicket(string PlayerId);

public sealed record SessionTicket(string Value);
// --8<-- [end:clientserver-contracts]

// --8<-- [start:fanout-contracts]
// Published without naming a recipient. Every subscribed node receives it.
public sealed record MaintenanceNotice(string Message);
// --8<-- [end:fanout-contracts]

// --8<-- [start:spot-contracts]
// Carried by the create call and handed to the room's create callback, which
// decides whether to accept the new room.
public sealed record OpenRoom(string Title);

public sealed record PostChat(string PlayerId, string Text);

public sealed record GetRoomState;

public sealed record RoomState(string Title, IReadOnlyList<string> Chat);
// --8<-- [end:spot-contracts]

// --8<-- [start:instance-spot-contracts]
// A match queue has no create call, so nothing here corresponds to OpenRoom.
public sealed record JoinMatchQueue(string PlayerId);

public sealed record MatchQueueStatus(int Waiting);
// --8<-- [end:instance-spot-contracts]

// --8<-- [start:actor-contracts]
// Handed to the lobby, which admits or rejects the new player.
public sealed record CreatePlayer(string Nickname);

public sealed record ChangeNickname(string Nickname);

public sealed record GetPlayer;

public sealed record PlayerInfo(string PlayerId, string Nickname);
// --8<-- [end:actor-contracts]

// --8<-- [start:stream-contracts]
// Exchanged over the external TCP connection, not between mesh nodes. The
// stream codec wants 64-bit integers as decimal strings, so the timestamp is
// carried as text rather than as a long.
public sealed record Ping(string SentAtUnixMs);

public sealed record Pong(string SentAtUnixMs);
// --8<-- [end:stream-contracts]

// --8<-- [start:session-actor-contracts]
public sealed record Authenticate(string PlayerId);

public sealed record Authenticated(string PlayerId);

// Pushed by the player to its own connection, with no request to answer.
public sealed record NicknameChanged(string Nickname);
// --8<-- [end:session-actor-contracts]

// --8<-- [start:node-direct-contracts]
// Answered by the node itself rather than by a channel, so the reply describes
// that one process.
public sealed record GetNodeStatus;

public sealed record NodeStatus(
    string MeshName,
    string? ChannelName,
    string CalledBy,
    string Uptime,
    int ProcessId);
// --8<-- [end:node-direct-contracts]
