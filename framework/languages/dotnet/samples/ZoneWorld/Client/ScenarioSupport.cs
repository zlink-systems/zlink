using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Client;

public sealed class ScenarioFailure(string message) : Exception(message);

/// <summary>
/// One connected client. It owns connection and player state; scenarios keep message waits at
/// their call sites so the public connector contract remains visible in the sample.
/// </summary>
public sealed class GameClient(IZlinkStreamConnector connector, string playerId) : IAsyncDisposable
{
    public IZlinkStreamConnector Connector { get; } = connector;

    public string PlayerId { get; } = playerId;

    public JoinWorldRes? Join { get; private set; }

    /// <summary>Where the walk starts. It follows the moves this client made, so a scenario can
    /// walk somewhere, act, and walk on from there.</summary>
    public (int X, int Y) Position { get; internal set; }

    public static async ValueTask<GameClient> ConnectAsync(
        ClientOptions options,
        string playerId,
        CancellationToken cancellationToken)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(options.GatewayEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(10),
            RequestTimeout = TimeSpan.FromSeconds(10),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxInboundObserverPayloadPreviewBytes = options.StreamTrace ? 2048 : 0
        });

        if (options.StreamTrace)
        {
            connector.WithInboundObserver((observation, _) =>
            {
                var preview = observation.PayloadPreview.IsEmpty
                    ? string.Empty
                    : Convert.ToBase64String(observation.PayloadPreview.Span);
                Console.Error.WriteLine(
                    "stream-trace inbound player={0} packet={1} kind={2} codec={3} "
                    + "length={4} compressed={5} preview_b64={6}",
                    playerId,
                    observation.Name,
                    observation.Kind,
                    observation.Codec,
                    observation.PayloadLength,
                    observation.IsCompressed,
                    preview);
                return ValueTask.CompletedTask;
            });
            connector.ErrorReceived += (error, _) =>
            {
                Console.Error.WriteLine(
                    "stream-trace error player={0} code={1} message={2}",
                    playerId,
                    error.Code,
                    error.Message);
                return ValueTask.CompletedTask;
            };
            connector.Disconnected += (disconnected, _) =>
            {
                Console.Error.WriteLine(
                    "stream-trace disconnected player={0} reason={1}",
                    playerId,
                    disconnected.CloseReason);
                return ValueTask.CompletedTask;
            };
        }

        await connector.Connect.Async(cancellationToken);
        return new GameClient(connector, playerId);
    }

    public async ValueTask<JoinWorldRes> JoinWorldAsync(CancellationToken cancellationToken)
    {
        Join = await Connector.Request(new JoinWorldReq(PlayerId))
            .Async<JoinWorldRes>(cancellationToken);
        Position = (Join.X, Join.Y);
        return Join;
    }

    public ValueTask MoveAsync(int x, int y) => Connector.Send(new MoveMsg(x, y)).Async();

    /// <summary>Builds legal movement steps without sending or waiting for messages. Each
    /// scenario keeps the connector wait and command order visible at its call site.</summary>
    public IReadOnlyList<(int X, int Y)> PlanWalkWithinZone(int targetX, int targetY)
    {
        if (ZoneKey(Position.X, Position.Y) != ZoneKey(targetX, targetY))
            throw new ScenarioFailure("A zone boundary requires an explicit ZoneChangedNotify wait.");

        var steps = new List<(int X, int Y)>();
        var (x, y) = Position;
        while (x != targetX || y != targetY)
        {
            var nextX = x + Math.Clamp(
                targetX - x,
                -ZoneWorldSpec.MaxStepPerAxis,
                ZoneWorldSpec.MaxStepPerAxis);
            var nextY = y + Math.Clamp(
                targetY - y,
                -ZoneWorldSpec.MaxStepPerAxis,
                ZoneWorldSpec.MaxStepPerAxis);
            if (CrossesBoth(x, y, nextX, nextY)) nextY = y;
            steps.Add((nextX, nextY));
            (x, y) = (nextX, nextY);
        }

        return steps;
    }

    public PlayerView Me(ZoneStateNotify state) =>
        state.Players.FirstOrDefault(p => p.PlayerId == PlayerId)
        ?? throw new ScenarioFailure($"Player '{PlayerId}' is missing from the zone state.");

    private static bool CrossesBoth(int fromX, int fromY, int toX, int toY)
    {
        var split = ZoneWorldSpec.ZoneSplit;
        return (fromX < split) != (toX < split) && (fromY < split) != (toY < split);
    }

    private static (bool East, bool South) ZoneKey(int x, int y) =>
        (x >= ZoneWorldSpec.ZoneSplit, y >= ZoneWorldSpec.ZoneSplit);

    public ValueTask DisposeAsync() => Connector.DisposeAsync();
}

/// <summary>The operator console's end of the world.</summary>
public sealed class OpsClient(IZlinkStreamConnector connector) : IAsyncDisposable
{
    public IZlinkStreamConnector Connector { get; } = connector;

    public static async ValueTask<OpsClient> ConnectAsync(string endpoint, CancellationToken cancellationToken)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(10),
            RequestTimeout = TimeSpan.FromSeconds(10),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async(cancellationToken);
        return new OpsClient(connector);
    }

    public ValueTask<WatchNodesRes> WatchNodesAsync(CancellationToken cancellationToken) =>
        Connector.Request(new WatchNodesReq()).Async<WatchNodesRes>(cancellationToken);

    public ValueTask<AnnounceWorldRes> AnnounceAsync(string text, CancellationToken cancellationToken) =>
        Connector.Request(new AnnounceWorldReq(text)).Async<AnnounceWorldRes>(cancellationToken);

    public ValueTask<SetMaintenanceRes> SetMaintenanceAsync(
        string nodeId,
        bool enabled,
        CancellationToken cancellationToken) =>
        Connector.Request(new SetMaintenanceReq(nodeId, enabled))
            .Async<SetMaintenanceRes>(cancellationToken);

    public ValueTask<NodeDiagnosticsRes> DiagnoseAsync(string nodeId, CancellationToken cancellationToken) =>
        Connector.Request(new NodeDiagnosticsReq(nodeId)).Async<NodeDiagnosticsRes>(cancellationToken);

    public ValueTask DisposeAsync() => Connector.DisposeAsync();
}

/// <summary>
/// Runner-only operational connection to the Gateway. It reads current object
/// locations but never uses the returned NodeRid as a message destination.
/// </summary>
public sealed class RelocationProbeClient(IZlinkStreamConnector connector) : IAsyncDisposable
{
    public static async ValueTask<RelocationProbeClient> ConnectAsync(
        string endpoint,
        CancellationToken cancellationToken)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(10),
            RequestTimeout = TimeSpan.FromSeconds(10),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async(cancellationToken);
        return new RelocationProbeClient(connector);
    }

    public ValueTask<RelocationPairRes> SelectPairAsync(CancellationToken cancellationToken) =>
        connector.Request(new RelocationPairReq()).Async<RelocationPairRes>(cancellationToken);

    public ValueTask<ActorLocationProbeRes> FindActorAsync(
        string actorId,
        CancellationToken cancellationToken) =>
        connector.Request(new ActorLocationProbeReq(actorId))
            .Async<ActorLocationProbeRes>(cancellationToken);

    public ValueTask<MessageFollowProbeRes> PrimeMessageFollowRouteAsync(
        string actorId,
        CancellationToken cancellationToken) =>
        connector.Request(new MessageFollowProbeReq(
                actorId,
                $"prime-{actorId}",
                "route-prime"u8.ToArray()))
            .Async<MessageFollowProbeRes>(cancellationToken);

    public ValueTask SendMessageFollowProbeAsync(
        string actorId,
        string probeId,
        byte[] payload) =>
        connector.Send(new MessageFollowProbeReq(actorId, probeId, payload)).Async();

    public ValueTask<MessageFollowProbeRes> RequestMessageFollowProbeAsync(
        string actorId,
        string probeId,
        byte[] payload,
        CancellationToken cancellationToken) =>
        connector.Request(new MessageFollowProbeReq(actorId, probeId, payload))
            .Async<MessageFollowProbeRes>(cancellationToken);

    public ValueTask DisposeAsync() => connector.DisposeAsync();
}

public sealed record ClientOptions(
    string GatewayEndpoint,
    string OpsEndpoint,
    bool StreamTrace)
{
    public static ClientOptions From(ZoneWorldClientSettings settings) =>
        new(settings.GatewayEndpoint, settings.OpsEndpoint, settings.StreamTrace);
}
