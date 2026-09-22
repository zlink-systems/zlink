namespace EngineLobby;

public sealed record Ping(string SentAtUnixMs);

public sealed record Pong(string SentAtUnixMs);

public sealed record Join(string Name);

public sealed record Joined(string ActorId, string Name);

public sealed record Chat(string Text);

public sealed record ChatNotify(string ActorId, string Name, string Text);
