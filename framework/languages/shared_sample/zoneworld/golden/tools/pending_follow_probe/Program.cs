using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using ZoneWorld.Client;
using ZoneWorld.Shared.Contracts;

var gateway = ReadOption(args, "--gateway");
using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
var cancellationToken = timeout.Token;
var playerId = $"golden-pending-{Guid.NewGuid():N}"[..23];

await using var player = await GameClient.ConnectAsync(
    new ClientOptions(gateway, "unused", StreamTrace: true, FaultArmFile: null),
    playerId,
    cancellationToken);
await using var probes = await RelocationProbeClient.ConnectAsync(gateway, cancellationToken);

var pair = await probes.SelectPairAsync(cancellationToken);
Ensure(pair.Error is null, "no adjacent cross-owner Zone pair was available");
Ensure(pair.SourceZoneId == ZoneIds.NorthWest,
    $"extraction expects the canonical source zone-nw, got {pair.SourceZoneId}");

var firstState = player.Connector.WaitFor<ZoneStateNotify>()
    .Where(message => message.Payload.Players.Any(candidate => candidate.PlayerId == playerId))
    .Timeout(TimeSpan.FromSeconds(15))
    .Async(cancellationToken);
await player.JoinWorldAsync(cancellationToken);
await firstState;

var before = await probes.FindActorAsync(playerId, cancellationToken);
Ensure(before.Error is null && before.OwnerNodeRid == pair.SourceOwnerNodeRid,
    "the new actor was not on the selected source owner");

var prime = await probes.PrimeMessageFollowRouteAsync(playerId, cancellationToken);
Ensure(prime.ProbeId == $"prime-{playerId}"
       && prime.Payload.AsSpan().SequenceEqual("route-prime"u8),
    "the source-owner route prime did not round-trip");

var (edge, across) = pair.TargetZoneId switch
{
    ZoneIds.NorthEast => ((X: 45, Y: 25), (X: 50, Y: 25)),
    ZoneIds.SouthWest => ((X: 25, Y: 45), (X: 25, Y: 50)),
    _ => throw new InvalidOperationException(
        $"unsupported canonical extraction edge {pair.SourceZoneId}->{pair.TargetZoneId}")
};

foreach (var step in player.PlanWalkWithinZone(edge.X, edge.Y))
    await MoveAndObserveAsync(player, step.X, step.Y, cancellationToken);

// Use independent public Gateway sessions so their request handlers can submit concurrently.
// The requests are staggered across the source relocation window; at least one therefore
// becomes a pending request whose reply completes after target cutover. ZoneWorld is exempt
// from the spec-30 host-maintenance move path, so this probe must complete through the spec-28
// in-memory relay window without accessing the configured Relocation Store prefix.
var requesters = new List<RelocationProbeClient>();
try
{
    for (var index = 0; index < 8; index++)
        requesters.Add(await RelocationProbeClient.ConnectAsync(gateway, cancellationToken));

    var changed = player.Connector.WaitFor<ZoneChangedNotify>()
        .Where(message => message.Payload.PlayerId == playerId
                          && message.Payload.ZoneId == pair.TargetZoneId)
        .Timeout(TimeSpan.FromSeconds(30))
        .Async(cancellationToken);
    await player.MoveAsync(across.X, across.Y);

    var requests = requesters.Select((requester, index) => RequestAfterDelayAsync(
        requester,
        playerId,
        index,
        TimeSpan.FromMilliseconds(8 + index * 4),
        cancellationToken)).ToArray();

    await changed;
    var replies = await Task.WhenAll(requests);
    Ensure(replies.All(result => result.Valid),
        "one or more pending Follow request replies lost payload/correlation");
}
finally
{
    foreach (var requester in requesters)
        await requester.DisposeAsync();
}

var after = await probes.FindActorAsync(playerId, cancellationToken);
Ensure(after.Error is null
       && after.OwnerNodeRid == pair.TargetOwnerNodeRid
       && after.OwnerNodeRid != before.OwnerNodeRid
       && after.ObjectGeneration == before.ObjectGeneration,
    "the actor did not complete the expected cross-owner relocation");

Console.WriteLine(
    "scenario ZW-B6-pending-cutover passed "
    + $"actor={playerId} source={before.OwnerNodeRid} target={after.OwnerNodeRid} "
    + $"generation={after.ObjectGeneration} requests=8");
return 0;

static async Task MoveAndObserveAsync(
    GameClient player,
    int x,
    int y,
    CancellationToken cancellationToken)
{
    var observed = player.Connector.WaitFor<ZoneStateNotify>()
        .Where(message => message.Payload.Players.Any(candidate =>
            candidate.PlayerId == player.PlayerId && candidate.X == x && candidate.Y == y))
        .Timeout(TimeSpan.FromSeconds(15))
        .Async(cancellationToken);
    await player.MoveAsync(x, y);
    await observed;
}

static async Task<(bool Valid, string ProbeId)> RequestAfterDelayAsync(
    RelocationProbeClient requester,
    string playerId,
    int index,
    TimeSpan delay,
    CancellationToken cancellationToken)
{
    await Task.Delay(delay, cancellationToken);
    var probeId = $"pending-{index}-{playerId}";
    var payload = Encoding.UTF8.GetBytes($"pending-payload-{index}");
    var reply = await requester.RequestMessageFollowProbeAsync(
        playerId,
        probeId,
        payload,
        cancellationToken);
    return (reply.ProbeId == probeId && reply.Payload.AsSpan().SequenceEqual(payload), probeId);
}

static string ReadOption(string[] arguments, string name)
{
    var index = Array.IndexOf(arguments, name);
    if (index < 0 || index + 1 == arguments.Length)
        throw new ArgumentException($"missing required option {name}");
    return arguments[index + 1];
}

static void Ensure(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
