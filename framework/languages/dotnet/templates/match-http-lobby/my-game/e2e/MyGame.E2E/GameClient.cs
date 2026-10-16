using System.Threading.Channels;
using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal sealed class GameClient(IZlinkStreamConnector connector, string playerId)
    : IAsyncDisposable
{
    internal static readonly TimeSpan ResponseTimeout = TimeSpan.FromSeconds(10);
    public IZlinkStreamConnector Connector { get; } = connector;
    public string PlayerId { get; } = playerId;
    public bool Disconnected { get; private set; }
    private readonly Dictionary<Type, Channel<object>> inboxes = [];

    public static async Task<GameClient> ConnectAsync(
        string endpoint,
        string playerId,
        CancellationToken ct
    )
    {
        var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                ConnectTimeout = ResponseTimeout,
                RequestTimeout = ResponseTimeout,
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            }
        );
        var client = new GameClient(connector, playerId);
        connector.OnDisconnected(
            (_, _) =>
            {
                client.Disconnected = true;
                return ValueTask.CompletedTask;
            }
        );
        connector.OnErrorReceived(
            (error, _) =>
            {
                Console.Error.WriteLine($"Client {playerId}: {error.Code}: {error.Message}");
                return ValueTask.CompletedTask;
            }
        );
        client.Record<RoomView>();
        client.Record<ShotRejected>();
        client.Record<MatchEnded>();
        client.Record<ActionRejected>();
        try
        {
            await connector.Connect.Async(ct);
            return client;
        }
        catch
        {
            await client.DisposeAsync();
            throw;
        }
    }

    private void Record<T>()
    {
        var inbox = Channel.CreateUnbounded<object>();
        inboxes.Add(typeof(T), inbox);
        Connector.On<T>(
            (message, _) =>
            {
                inbox.Writer.TryWrite(message.Payload!);
                return ValueTask.CompletedTask;
            }
        );
    }

    public async Task<T> WaitAsync<T>(Func<T, bool> predicate, CancellationToken ct)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(ResponseTimeout);
        try
        {
            await foreach (var message in inboxes[typeof(T)].Reader.ReadAllAsync(deadline.Token))
                if (predicate((T)message))
                    return (T)message;
        }
        catch (OperationCanceledException error) when (!ct.IsCancellationRequested)
        {
            throw new TimeoutException(
                $"Player {PlayerId}: timed out waiting for {typeof(T).Name}.",
                error
            );
        }
        throw new InvalidOperationException("Client inbox closed.");
    }

    public Task SendAsync<T>(T message, CancellationToken ct) =>
        Connector.Send(message).Async(ct).AsTask();

    public Task JoinAsync(string roomId, CancellationToken ct) =>
        SendAsync(new JoinRoomRequest(PlayerId, roomId), ct);

    public async Task<RoomView> ViewAsync(Func<RoomView, bool> predicate, CancellationToken ct)
    {
        var response = WaitAsync<RoomView>(predicate, ct);
        await SendAsync(new RoomViewRequest(), ct);
        return await response;
    }

    public async Task<SessionInfo> SessionAsync(CancellationToken ct) =>
        await Connector.Request(new SessionInfoRequest()).Async<SessionInfo>(ct);

    public ValueTask DisposeAsync() => RunOutcome.CleanupAsync(Connector.DisposeAsync);
}
