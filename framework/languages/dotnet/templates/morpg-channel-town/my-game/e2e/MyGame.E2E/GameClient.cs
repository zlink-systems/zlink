using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal sealed class GameClient(IZlinkStreamConnector connector, string playerId)
    : IAsyncDisposable
{
    private static readonly TimeSpan MessageTimeout = TimeSpan.FromSeconds(10);
    public IZlinkStreamConnector Connector { get; } = connector;
    public string PlayerId { get; } = playerId;
    public bool Disconnected { get; private set; }

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
                ConnectTimeout = MessageTimeout,
                RequestTimeout = MessageTimeout,
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

    public async Task<T> ReceiveAsync<T>(Func<T, bool> predicate, CancellationToken ct)
    {
        var result = await Connector
            .WaitFor<T>()
            .Where(message => predicate(message.Payload))
            .Timeout(MessageTimeout)
            .Async(ct);
        return result.Payload;
    }

    public async Task<T> SendAsync<T, TRequest>(
        TRequest request,
        Func<T, bool> predicate,
        CancellationToken ct
    )
    {
        var response = ReceiveAsync(predicate, ct);
        await Connector.Send(request).Async(ct);
        return await response;
    }

    public Task<TownView> JoinAsync(int channel, CancellationToken ct) =>
        SendAsync<TownView, JoinTownRequest>(
            new(PlayerId, channel),
            view => view.TownId == GameNames.TownId(channel) && view.Players.Contains(PlayerId),
            ct
        );

    public Task<SessionInfo> SessionAsync(CancellationToken ct) =>
        Connector.Request(new SessionInfoRequest()).Async<SessionInfo>(ct).AsTask();

    internal static bool SameDungeonState(DungeonView before, DungeonView after) =>
        before.DungeonId == after.DungeonId
        && before.PartyId == after.PartyId
        && before.Stage == after.Stage
        && before.Members.SequenceEqual(after.Members);

    public Task<DungeonView> DungeonAsync(
        DungeonView expected,
        CancellationToken ct,
        bool moved = false
    ) =>
        SendAsync<DungeonView, ViewRequest>(
            new(),
            view =>
                SameDungeonState(expected, view)
                && (moved ? view.NodeId != expected.NodeId : view.NodeId == expected.NodeId),
            ct
        );

    public ValueTask DisposeAsync() => RunOutcome.CleanupAsync(Connector.DisposeAsync);
}
