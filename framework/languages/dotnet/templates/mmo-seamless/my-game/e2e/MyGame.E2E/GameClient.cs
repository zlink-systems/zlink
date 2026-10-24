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

    public async Task<WorldState> JoinAsync(int x, CancellationToken ct)
    {
        var response = Connector
            .WaitFor<WorldState>()
            .Where(message => message.Payload.ActorId == PlayerId)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await Connector.Send(new JoinWorldRequest(PlayerId, x)).Async(ct);
        return (await response).Payload;
    }

    public async Task<WorldState> MoveAsync(int x, int sequence, CancellationToken ct)
    {
        var response = Connector
            .WaitFor<WorldState>()
            .Where(message =>
                message.Payload.ActorId == PlayerId && message.Payload.Sequence == sequence
            )
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(ct);
        await Connector.Send(new MoveRequest(x, sequence)).Async(ct);
        return (await response).Payload;
    }

    public async Task<string> SessionAsync(CancellationToken ct) =>
        (await Connector.Request(new SessionInfoRequest()).Async<SessionInfo>(ct)).SessionId;

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

    public ValueTask DisposeAsync() => outcome.CleanupAsync(Connector.DisposeAsync);
}
