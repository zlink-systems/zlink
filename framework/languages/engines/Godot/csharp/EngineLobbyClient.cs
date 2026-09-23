using Systems.Zlink.Stream.Connector.Contracts;

namespace EngineLobby;

public sealed class EngineLobbyClient : IAsyncDisposable
{
    private readonly IZlinkStreamConnector _connector;
    private readonly IDisposable _chatSubscription;

    public EngineLobbyClient(string endpoint)
    {
        _connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                DispatchMode = ZlinkStreamDispatchMode.Manual,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        _chatSubscription = _connector.On<ChatNotify>(
            (message, _) =>
            {
                ChatReceived?.Invoke(message.Payload);
                return ValueTask.CompletedTask;
            }
        );
    }

    public event Action<ChatNotify>? ChatReceived;

    public JoinRes? Joined { get; private set; }

    public bool IsConnected => _connector.IsConnected;

    public async Task ConnectAndJoinAsync(string name)
    {
        await _connector.Connect.Async();

        const string sentAt = "1000";
        var pong = await _connector.Request(new PingReq(sentAt)).Async<PingRes>();
        if (pong.SentAtUnixMs != sentAt)
            throw new InvalidOperationException("PingRes did not echo sentAtUnixMs.");

        var joined = await _connector.Request(new JoinReq(name)).Async<JoinRes>();
        if (joined.Name != name || string.IsNullOrEmpty(joined.ActorId))
            throw new InvalidOperationException(
                "JoinRes did not confirm the requested participant."
            );

        Joined = joined;
    }

    public ValueTask SendChatAsync(string text) => _connector.Send(new ChatMsg(text)).Async();

    public ValueTask DispatchAsync() => _connector.Dispatch.Async();

    public async ValueTask DisposeAsync()
    {
        _chatSubscription.Dispose();
        if (_connector.IsConnected)
            await _connector.Close.Async();
        await _connector.DisposeAsync();
    }
}
