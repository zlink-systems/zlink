namespace EngineLobby;

public sealed record PingReq(string SentAtUnixMs);

public sealed record PingRes(string SentAtUnixMs);

public sealed record JoinReq(string Name);

public sealed record JoinRes(string ActorId, string Name);

public sealed record ChatMsg(string Text);

public sealed record ChatNotify(string ActorId, string Name, string Text);
