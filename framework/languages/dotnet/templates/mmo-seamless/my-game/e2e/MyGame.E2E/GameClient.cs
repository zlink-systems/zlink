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
    public IZlinkStreamConnector Connector { get; } = connector;
    public string PlayerId { get; } = playerId;
    public bool Disconnected { get; private set; }
    private bool disposed;

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
                ConnectTimeout = TimeSpan.FromSeconds(10),
                RequestTimeout = TimeSpan.FromSeconds(10),
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

    public async Task<WorldState> JoinAsync(double x, CancellationToken ct)
    {
        var response = Connector
            .WaitFor<WorldState>()
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await Connector.Send(new JoinWorldRequest(PlayerId, x)).Async(ct);
        return (await response).Payload;
    }

    public async Task<WorldState> MoveAsync(double delta, long sequence, CancellationToken ct)
    {
        var response = WaitForMoveAsync(sequence, ct);
        await SendMoveAsync(delta, sequence, ct);
        return await response;
    }

    public async Task<WorldState> WaitForMoveAsync(long sequence, CancellationToken ct)
    {
        var response = Connector
            .WaitFor<WorldState>()
            .Where(message => message.Payload.Sequence == sequence)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        return (await response).Payload;
    }

    public async Task<ActionRejected> WaitForRejectionAsync(string reason, CancellationToken ct)
    {
        var response = Connector
            .WaitFor<ActionRejected>()
            .Where(message => message.Payload.Reason == reason)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        return (await response).Payload;
    }

    public async Task SendMoveAsync(double delta, long sequence, CancellationToken ct) =>
        await Connector.Send(new MoveRequest(delta, sequence)).Async(ct);

    public async Task<ActionRejected> MoveRejectedAsync(
        double delta,
        long sequence,
        CancellationToken ct
    )
    {
        var response = Connector
            .WaitFor<ActionRejected>()
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await Connector.Send(new MoveRequest(delta, sequence)).Async(ct);
        return (await response).Payload;
    }

    public async Task<WorldState> MoveWithBudgetRetryAsync(
        double delta,
        long sequence,
        CancellationToken ct
    )
    {
        while (true)
        {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(ct);
            var moved = Connector
                .WaitFor<WorldState>()
                .Where(message => message.Payload.Sequence == sequence)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(attempt.Token)
                .AsTask();
            var rejected = Connector
                .WaitFor<ActionRejected>()
                .Where(message => message.Payload.Reason == "move-budget")
                .Timeout(TimeSpan.FromSeconds(10))
                .Async(attempt.Token)
                .AsTask();
            await Connector.Send(new MoveRequest(delta, sequence)).Async(ct);
            var completed = await Task.WhenAny(moved, rejected);
            if (completed == moved)
            {
                attempt.Cancel();
                return (await moved).Payload;
            }
            var rejection = (await rejected).Payload;
            attempt.Cancel();
            if (rejection.RetryAfterMs <= 0)
                throw new InvalidOperationException("move-budget must report a positive retry delay.");
            await Task.Delay(TimeSpan.FromMilliseconds(rejection.RetryAfterMs), ct);
        }
    }

    public async Task<ActionRejected> PacketRejectedAsync<TMessage>(TMessage packet, CancellationToken ct)
        where TMessage : notnull
    {
        var response = Connector
            .WaitFor<ActionRejected>()
            .Where(message => message.Payload.Reason == "packet-not-allowed")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await Connector.Send(packet).Async(ct);
        return (await response).Payload;
    }

    public async Task<WorldState> StateAsync(CancellationToken ct)
    {
        var response = Connector.WaitFor<WorldState>().Timeout(TimeSpan.FromSeconds(10)).Async(ct);
        await Connector.Send(new JoinWorldRequest(PlayerId, 0)).Async(ct);
        return (await response).Payload;
    }

    public async Task<SessionInfo> SessionAsync(CancellationToken ct) =>
        await Connector.Request(new SessionInfoRequest()).Async<SessionInfo>(ct);

    public async Task<DungeonOpened> DungeonAsync(string dungeonId, CancellationToken ct)
    {
        var response = Connector
            .WaitFor<DungeonOpened>()
            .Where(message => message.Payload.DungeonId == dungeonId)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await Connector.Send(new OpenDungeonRequest(dungeonId)).Async(ct);
        return (await response).Payload;
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
            return;
        disposed = true;
        await outcome.CleanupAsync(Connector.DisposeAsync);
    }
}
