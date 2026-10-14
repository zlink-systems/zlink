using System.Collections.Concurrent;
using System.Threading.Channels;
using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal sealed class GameClient(IZlinkStreamConnector connector, string playerId)
    : IAsyncDisposable
{
    private static readonly TimeSpan ResponseTimeout = TimeSpan.FromSeconds(10);
    public IZlinkStreamConnector Connector { get; } = connector;
    public string PlayerId { get; } = playerId;
    public bool Disconnected { get; private set; }
    public ConcurrentQueue<object> Received { get; } = new();
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
        client.Record<TableView>();
        client.Record<HandDealt>();
        client.Record<HandResult>();
        client.Record<ActionRejected>();
        try
        {
            await connector.Connect.Async(ct);
            return client;
        }
        catch
        {
            await connector.DisposeAsync();
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
                Received.Enqueue(message.Payload!);
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

    public async Task<TableView> JoinAsync(string tableId, CancellationToken ct)
    {
        var view = WaitAsync<TableView>(
            view => view.TableId == tableId && view.ActorId == PlayerId,
            ct
        );
        await SendAsync(new JoinTableRequest(PlayerId, tableId), ct);
        return await view;
    }

    public async Task<TableView> ViewAsync(Func<TableView, bool> predicate, CancellationToken ct)
    {
        var response = WaitAsync<TableView>(predicate, ct);
        await SendAsync(new TableViewRequest(), ct);
        return await response;
    }

    public async Task<string> SessionAsync(CancellationToken ct) =>
        (await Connector.Request(new SessionInfoRequest()).Async<SessionInfo>(ct)).SessionId;

    public ValueTask DisposeAsync() => Connector.DisposeAsync();
}
