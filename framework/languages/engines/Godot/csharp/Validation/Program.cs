using EngineLobby;

if (args.Length != 1)
    throw new ArgumentException("Pass the Engine Lobby WebSocket endpoint.");

await using var alice = new EngineLobbyClient(args[0]);
await using var bob = new EngineLobbyClient(args[0]);

await alice.ConnectAndJoinAsync("alice");
await bob.ConnectAndJoinAsync("bob");

if (alice.Joined!.ActorId == bob.Joined!.ActorId)
    throw new InvalidOperationException("JoinRes actor IDs must be distinct.");

ChatNotify? aliceNotification = null;
ChatNotify? bobNotification = null;
alice.ChatReceived += chat => aliceNotification = chat;
bob.ChatReceived += chat => bobNotification = chat;

await alice.SendChatAsync("hello");

using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
while (aliceNotification is null || bobNotification is null)
{
    deadline.Token.ThrowIfCancellationRequested();
    await alice.DispatchAsync();
    await bob.DispatchAsync();
    await Task.Delay(10, deadline.Token);
}

foreach (var notification in new[] { aliceNotification, bobNotification })
{
    if (
        notification.ActorId != alice.Joined.ActorId
        || notification.Name != "alice"
        || notification.Text != "hello"
    )
        throw new InvalidOperationException("ChatNotify payload mismatch.");
}

Console.WriteLine("godot-engine-lobby-validation=ok");
