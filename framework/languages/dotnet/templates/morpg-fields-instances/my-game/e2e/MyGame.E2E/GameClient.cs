using System.Threading.Channels;
using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal sealed class GameClient(
    RunOutcome outcome,
    IZlinkStreamConnector connector,
    string playerId
) : IAsyncDisposable
{
    private static readonly TimeSpan MessageTimeout = TimeSpan.FromSeconds(10);
    public IZlinkStreamConnector Connector { get; } = connector;
    public string PlayerId { get; } = playerId;
    private readonly Dictionary<Type, Channel<object>> inboxes = [];
    public bool Disconnected { get; private set; }

    public static async Task<GameClient> ConnectAsync(
        RunOutcome outcome,
        string endpoint,
        string playerId,
        CancellationToken ct
    )
    {
        var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                ConnectTimeout = MessageTimeout,
                RequestTimeout = MessageTimeout,
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            }
        );
        var client = new GameClient(outcome, connector, playerId);
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
        client.Record<WorldState>();
        client.Record<BorderVisible>();
        client.Record<PartyView>();
        client.Record<DungeonView>();
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
        Connector.On<T>((message, _) =>
        {
            inbox.Writer.TryWrite(message.Payload!);
            return ValueTask.CompletedTask;
        });
    }

    public async Task<T> ReceiveAsync<T>(Func<T, bool> predicate, CancellationToken ct)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(ct);
        deadline.CancelAfter(MessageTimeout);
        try
        {
            await foreach (var message in inboxes[typeof(T)].Reader.ReadAllAsync(deadline.Token))
                if (predicate((T)message)) return (T)message;
        }
        catch (OperationCanceledException error) when (!ct.IsCancellationRequested)
        {
            throw new TimeoutException($"Player {PlayerId}: timed out waiting for {typeof(T).Name}.", error);
        }
        throw new InvalidOperationException("Client inbox closed.");
    }

    public async Task<T> SendAsync<T, TRequest>(
        TRequest request,
        Func<T, bool> predicate,
        CancellationToken ct
    )
    {
        while (inboxes[typeof(T)].Reader.TryRead(out _)) { }
        var response = ReceiveAsync(predicate, ct);
        await Connector.Send(request).Async(ct);
        return await response;
    }

    public Task<WorldState> JoinAsync(int x, CancellationToken ct) =>
        SendAsync<WorldState, JoinWorldRequest>(
            new(PlayerId, x),
            view => view.FieldId == GameNames.FieldId(x),
            ct
        );

    public Task<SessionInfo> SessionAsync(CancellationToken ct) =>
        Connector.Request(new SessionInfoRequest()).Async<SessionInfo>(ct).AsTask();

    public Task<DungeonView> DungeonAsync(CancellationToken ct, string? previousHost = null) =>
        SendAsync<DungeonView, ViewRequest>(new(), view => view.NodeId != previousHost, ct);

    public ValueTask DisposeAsync() => outcome.CleanupAsync(Connector.DisposeAsync);
}
